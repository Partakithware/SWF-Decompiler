#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <map>
#include <list>
#include <algorithm>

// ==========================================
// Basic Structures
// ==========================================

struct RGBA {
    uint8_t r, g, b, a;
    RGBA() : r(0), g(0), b(0), a(255) {}
    RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) : r(r), g(g), b(b), a(a) {}
};

struct Point {
    double x, y;
    
    Point(double x = 0, double y = 0) : x(x), y(y) {}

    bool operator==(const Point& other) const {
        return std::abs(x - other.x) < 0.0001 && std::abs(y - other.y) < 0.0001;
    }
};

struct Matrix {
    double a, b, c, d, tx, ty;
    Matrix() : a(1), b(0), c(0), d(1), tx(0), ty(0) {}
};

struct Rect {
    int32_t xMin, xMax, yMin, yMax;
    Rect() : xMin(0), xMax(0), yMin(0), yMax(0) {}
};

// ==========================================
// Style Definitions
// ==========================================

struct FillStyle {
    int type; // 0=solid, 1=linear, 2=radial, 3=bitmap
    RGBA color;
    Matrix matrix;
    std::vector<RGBA> gradientColors;
    std::vector<uint8_t> gradientRatios;
};

struct LineStyle {
    uint16_t width;
    RGBA color;
    int startCap; // 0=round, 1=none, 2=square
    int endCap;
    int joinStyle; // 0=round, 1=bevel, 2=miter
    uint16_t miterLimit;
    bool hasFill;
    FillStyle fillStyle;

    LineStyle() : width(20), startCap(0), endCap(0), joinStyle(0), miterLimit(0), hasFill(false) {}
};

// ==========================================
// Geometry / Edge Handling
// ==========================================

struct Edge {
    Point p1;
    Point p2;
    Point control; // For quadratic curves
    bool isQuad;
    
    Edge reversed() const {
        Edge e;
        e.p1 = p2;
        e.p2 = p1;
        e.control = control;
        e.isQuad = isQuad;
        return e;
    }
};

// ==========================================
// Bit Manipulation
// ==========================================

class BitReader {
    const uint8_t* data;
    size_t bytePos;
    int bitPos;
    size_t size;

public:
    BitReader(const uint8_t* d, size_t s) : data(d), bytePos(0), bitPos(0), size(s) {}
    
    uint32_t readBits(int numBits) {
        uint32_t result = 0;
        for (int i = 0; i < numBits; i++) {
            if (bytePos >= size) return result;
            int bit = (data[bytePos] >> (7 - bitPos)) & 1;
            result = (result << 1) | bit;
            bitPos++;
            if (bitPos == 8) {
                bitPos = 0;
                bytePos++;
            }
        }
        return result;
    }
    
    int32_t readSignedBits(int numBits) {
        uint32_t val = readBits(numBits);
        if (val & (1 << (numBits - 1))) {
            return val | (0xFFFFFFFF << numBits); 
        }
        return val;
    }
    
    void alignByte() {
        if (bitPos != 0) {
            bitPos = 0;
            bytePos++;
        }
    }
    
    size_t getBytePos() const { return bytePos; }
    void setBytePos(size_t pos) { bytePos = pos; bitPos = 0; }
};

// ==========================================
// Renderer
// ==========================================

class ShapeRenderer {
    std::vector<uint8_t> data;
    std::stringstream svgContent;
    std::stringstream defsContent;
    
    int shapeVersion;
    Rect bounds;
    int globalGradientCount;
    
    std::map<int, std::list<Edge>> fillLayers;
    std::map<int, std::list<Edge>> strokeLayers;
    
    std::vector<FillStyle> activeFillStyles;
    std::vector<LineStyle> activeLineStyles;

    uint16_t readU16(BitReader& br) {
        br.alignByte();
        size_t pos = br.getBytePos();
        if (pos + 2 > data.size()) return 0;
        uint16_t val = data[pos] | (data[pos+1] << 8);
        br.setBytePos(pos + 2);
        return val;
    }
    
    uint8_t readU8(BitReader& br) {
        br.alignByte();
        size_t pos = br.getBytePos();
        if (pos >= data.size()) return 0;
        br.setBytePos(pos + 1);
        return data[pos];
    }

    Rect readRect(BitReader& br) {
        Rect r;
        int nBits = br.readBits(5);
        r.xMin = br.readSignedBits(nBits);
        r.xMax = br.readSignedBits(nBits);
        r.yMin = br.readSignedBits(nBits);
        r.yMax = br.readSignedBits(nBits);
        br.alignByte();
        return r;
    }

    Matrix readMatrix(BitReader& br) {
        Matrix m;
        bool hasScale = br.readBits(1);
        if (hasScale) {
            int nScaleBits = br.readBits(5);
            m.a = br.readSignedBits(nScaleBits) / 65536.0;
            m.d = br.readSignedBits(nScaleBits) / 65536.0;
        }
        bool hasRotate = br.readBits(1);
        if (hasRotate) {
            int nRotateBits = br.readBits(5);
            m.c = br.readSignedBits(nRotateBits) / 65536.0;
            m.b = br.readSignedBits(nRotateBits) / 65536.0;
        }
        int nTranslateBits = br.readBits(5);
        m.tx = br.readSignedBits(nTranslateBits) / 20.0;
        m.ty = br.readSignedBits(nTranslateBits) / 20.0;
        return m;
    }

    RGBA readRGB(BitReader& br) {
        RGBA c;
        c.r = readU8(br);
        c.g = readU8(br);
        c.b = readU8(br);
        return c;
    }

    RGBA readRGBA(BitReader& br) {
        RGBA c;
        c.r = readU8(br);
        c.g = readU8(br);
        c.b = readU8(br);
        c.a = readU8(br);
        return c;
    }

    // ==========================================
    // SVG Generation Helpers
    // ==========================================

    std::string defineGradient(const FillStyle& fill) {
        int id = globalGradientCount++;
        std::stringstream ss;
        
        std::string gradType = (fill.type == 2) ? "radialGradient" : "linearGradient";
        
        ss << "<" << gradType << " id=\"grad" << id << "\" "
           << "gradientUnits=\"userSpaceOnUse\" ";
           
        if (fill.type == 2) {
            ss << "cx=\"0\" cy=\"0\" r=\"16384\" fx=\"0\" fy=\"0\" ";
        } else {
            ss << "x1=\"-16384\" y1=\"0\" x2=\"16384\" y2=\"0\" ";
        }

        ss << "gradientTransform=\"matrix(" 
           << fill.matrix.a << "," << fill.matrix.b << "," 
           << fill.matrix.c << "," << fill.matrix.d << "," 
           << fill.matrix.tx << "," << fill.matrix.ty << ")\">\n";

        for (size_t i = 0; i < fill.gradientColors.size(); i++) {
            RGBA c = fill.gradientColors[i];
            double offset = fill.gradientRatios[i] / 255.0;
            ss << "  <stop offset=\"" << offset << "\" "
               << "stop-color=\"rgb(" << (int)c.r << "," << (int)c.g << "," << (int)c.b << ")\" "
               << "stop-opacity=\"" << (c.a / 255.0) << "\"/>\n";
        }
        
        ss << "</" << gradType << ">\n";
        defsContent << ss.str();
        
        return "url(#grad" + std::to_string(id) + ")";
    }

    // Added isFill param to support closing paths
    std::string edgesToPathD(std::list<Edge>& edges, bool isFill) {
        if (edges.empty()) return "";
        std::stringstream path;
        
        // Increase precision to avoid rounding gaps
        path << std::fixed << std::setprecision(4);
        
        while (!edges.empty()) {
            Edge current = edges.front();
            edges.pop_front();
            
            path << "M " << current.p1.x << " " << current.p1.y << " ";
            
            if (current.isQuad) {
                path << "Q " << current.control.x << " " << current.control.y << " " 
                     << current.p2.x << " " << current.p2.y << " ";
            } else {
                path << "L " << current.p2.x << " " << current.p2.y << " ";
            }
            
            Point tip = current.p2;
            bool found = true;
            while (found) {
                found = false;
                for (auto it = edges.begin(); it != edges.end(); ++it) {
                    if (it->p1 == tip) {
                        if (it->isQuad) {
                            path << "Q " << it->control.x << " " << it->control.y << " " 
                                 << it->p2.x << " " << it->p2.y << " ";
                        } else {
                            path << "L " << it->p2.x << " " << it->p2.y << " ";
                        }
                        tip = it->p2;
                        edges.erase(it);
                        found = true;
                        break;
                    }
                }
            }
            // Explicitly close loop for fills to help renderer
            if (isFill) {
                path << "Z ";
            } else {
                path << " ";
            }
        }
        return path.str();
    }

    void flushLayers() {
        // Render Fills
        for (auto& pair : fillLayers) {
            int styleIdx = pair.first;
            std::list<Edge>& edges = pair.second;
            if (edges.empty()) continue;
            
            if (styleIdx < 1 || styleIdx > (int)activeFillStyles.size()) continue;
            const FillStyle& fs = activeFillStyles[styleIdx - 1];
            
            std::string fillVal;
            std::string opacityVal;
            
            if (fs.type == 0) {
                fillVal = "rgb(" + std::to_string(fs.color.r) + "," + 
                          std::to_string(fs.color.g) + "," + std::to_string(fs.color.b) + ")";
                opacityVal = std::to_string(fs.color.a/255.0);
            } else if (fs.type == 1 || fs.type == 2) {
                fillVal = defineGradient(fs);
                opacityVal = "1";
            } else {
                fillVal = "#CCCCCC"; 
                opacityVal = "1";
            }
            
            // KEY FIX: Add a tiny stroke of the same color to bridge antialiasing gaps
            svgContent << "<path d=\"" << edgesToPathD(edges, true) << "\" " 
                       << "fill=\"" << fillVal << "\" "
                       << "fill-opacity=\"" << opacityVal << "\" "
                       << "stroke=\"" << fillVal << "\" "     // Stroke matches fill
                       << "stroke-opacity=\"" << opacityVal << "\" "
                       << "stroke-width=\"0.05\" "            // Tiny width (approx 0.05px)
                       << "stroke-linecap=\"round\" "
                       << "stroke-linejoin=\"round\" "
                       << "fill-rule=\"nonzero\" />\n";
        }
        
        // Render Strokes
        for (auto& pair : strokeLayers) {
            int styleIdx = pair.first;
            std::list<Edge>& edges = pair.second;
            if (edges.empty()) continue;
            
            if (styleIdx < 1 || styleIdx > (int)activeLineStyles.size()) continue;
            const LineStyle& ls = activeLineStyles[styleIdx - 1];
            
            std::string strokeAttr = "fill=\"none\" stroke=\"rgb(" + 
                                     std::to_string(ls.color.r) + "," + 
                                     std::to_string(ls.color.g) + "," + 
                                     std::to_string(ls.color.b) + ")\" " +
                                     "stroke-opacity=\"" + std::to_string(ls.color.a/255.0) + "\" " +
                                     "stroke-width=\"" + std::to_string(std::max(1.0, (double)ls.width/20.0)) + "\"";

            if (ls.startCap == 1) strokeAttr += " stroke-linecap=\"butt\"";
            else if (ls.startCap == 2) strokeAttr += " stroke-linecap=\"square\"";
            else strokeAttr += " stroke-linecap=\"round\"";
            
            if (ls.joinStyle == 1) strokeAttr += " stroke-linejoin=\"bevel\"";
            else if (ls.joinStyle == 2) {
                strokeAttr += " stroke-linejoin=\"miter\"";
                strokeAttr += " stroke-miterlimit=\"" + std::to_string(ls.miterLimit/20.0) + "\"";
            } else {
                strokeAttr += " stroke-linejoin=\"round\"";
            }

            svgContent << "<path d=\"" << edgesToPathD(edges, false) << "\" " 
                       << strokeAttr << " />\n";
        }

        fillLayers.clear();
        strokeLayers.clear();
    }

    void readFillStyles(BitReader& br, bool hasAlpha) {
        activeFillStyles.clear();
        uint16_t count = readU8(br);
        if (count == 0xFF && shapeVersion >= 2) {
            count = readU16(br);
        }
        
        for (int i = 0; i < count; i++) {
            FillStyle fill;
            uint8_t fillType = readU8(br);
            
            if (fillType == 0x00) {
                fill.type = 0;
                fill.color = hasAlpha ? readRGBA(br) : readRGB(br);
            } else if (fillType == 0x10 || fillType == 0x12 || fillType == 0x13) {
                fill.type = (fillType == 0x10) ? 1 : 2;
                fill.matrix = readMatrix(br);
                br.alignByte();
                br.readBits(2); // spread
                br.readBits(2); // interp
                uint8_t numGradients = br.readBits(4);
                for (int k = 0; k < numGradients; k++) {
                    fill.gradientRatios.push_back(readU8(br));
                    fill.gradientColors.push_back(hasAlpha ? readRGBA(br) : readRGB(br));
                }
                if (shapeVersion >= 4 && fillType == 0x13) readU16(br);
            } else if (fillType >= 0x40) {
                fill.type = 3;
                uint16_t bitmapId = readU16(br);
                fill.matrix = readMatrix(br);
                br.alignByte();
            }
            activeFillStyles.push_back(fill);
        }
    }

    void readLineStyles(BitReader& br, bool hasAlpha) {
        activeLineStyles.clear();
        uint16_t count = readU8(br);
        if (count == 0xFF && shapeVersion >= 2) {
            count = readU16(br);
        }
        
        for (int i = 0; i < count; i++) {
            LineStyle style;
            style.width = readU16(br);
            
            if (shapeVersion >= 4) {
                style.startCap = br.readBits(2);
                style.joinStyle = br.readBits(2);
                style.hasFill = br.readBits(1);
                br.readBits(1);
                br.readBits(1);
                br.readBits(1);
                br.readBits(5);
                br.readBits(1);
                style.endCap = br.readBits(2);
                br.alignByte();
                
                if (style.joinStyle == 2) style.miterLimit = readU16(br);
                
                if (style.hasFill) {
                    readFillStyles(br, hasAlpha); 
                    style.color = RGBA(0,0,0);
                } else {
                    style.color = readRGBA(br);
                }
            } else {
                style.color = hasAlpha ? readRGBA(br) : readRGB(br);
            }
            activeLineStyles.push_back(style);
        }
    }

public:
    ShapeRenderer(int version) : shapeVersion(version), globalGradientCount(0) {}
    
    bool loadShape(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) return false;
        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        data.resize(fileSize);
        file.read((char*)data.data(), fileSize);
        return true;
    }
    
    bool renderToSVG(const std::string& outputFile) {
        BitReader br(data.data(), data.size());
        
        readU16(br); // ID
        bounds = readRect(br);
        
        if (shapeVersion == 4) {
            readRect(br);
            br.readBits(5);
            br.readBits(1);
            br.readBits(1);
            br.readBits(1);
            br.alignByte();
        }
        
        bool hasAlpha = (shapeVersion >= 3);
        
        readFillStyles(br, hasAlpha);
        readLineStyles(br, hasAlpha);
        
        int numFillBits = br.readBits(4);
        int numLineBits = br.readBits(4);
        
        Point currentPos(0, 0);
        int currentFill0 = 0;
        int currentFill1 = 0;
        int currentLine = 0;
        
        while(true) {
            bool typeFlag = br.readBits(1);
            if (!typeFlag) {
                uint32_t flags = br.readBits(5);
                if (flags == 0) break;
                
                if (flags & 0x01) {
                    int nBits = br.readBits(5);
                    int32_t x = br.readSignedBits(nBits);
                    int32_t y = br.readSignedBits(nBits);
                    currentPos.x = x / 20.0;
                    currentPos.y = y / 20.0;
                }
                if (flags & 0x02) currentFill0 = br.readBits(numFillBits);
                if (flags & 0x04) currentFill1 = br.readBits(numFillBits);
                if (flags & 0x08) currentLine = br.readBits(numLineBits);
                
                if (flags & 0x10) {
                    flushLayers();
                    readFillStyles(br, hasAlpha);
                    readLineStyles(br, hasAlpha);
                    numFillBits = br.readBits(4);
                    numLineBits = br.readBits(4);
                }
            } else {
                Edge edge;
                edge.p1 = currentPos;
                
                bool straightFlag = br.readBits(1);
                int numBits = br.readBits(4) + 2;
                
                if (straightFlag) {
                    bool generalLine = br.readBits(1);
                    int32_t dx = 0, dy = 0;
                    if (generalLine) {
                        dx = br.readSignedBits(numBits);
                        dy = br.readSignedBits(numBits);
                    } else {
                        bool vert = br.readBits(1);
                        if (vert) dy = br.readSignedBits(numBits);
                        else dx = br.readSignedBits(numBits);
                    }
                    edge.p2.x = currentPos.x + dx / 20.0;
                    edge.p2.y = currentPos.y + dy / 20.0;
                    edge.isQuad = false;
                } else {
                    int32_t cdx = br.readSignedBits(numBits);
                    int32_t cdy = br.readSignedBits(numBits);
                    int32_t adx = br.readSignedBits(numBits);
                    int32_t ady = br.readSignedBits(numBits);
                    
                    edge.control.x = currentPos.x + cdx / 20.0;
                    edge.control.y = currentPos.y + cdy / 20.0;
                    edge.p2.x = edge.control.x + adx / 20.0;
                    edge.p2.y = edge.control.y + ady / 20.0;
                    edge.isQuad = true;
                }
                
                if (currentFill0 != 0) {
                    fillLayers[currentFill0].push_back(edge.reversed());
                }
                if (currentFill1 != 0) {
                    fillLayers[currentFill1].push_back(edge);
                }
                if (currentLine != 0) {
                    strokeLayers[currentLine].push_back(edge);
                }
                
                currentPos = edge.p2;
            }
        }
        
        flushLayers();
        
        std::ofstream out(outputFile);
        if (!out.is_open()) return false;
        
        double w = (bounds.xMax - bounds.xMin) / 20.0;
        double h = (bounds.yMax - bounds.yMin) / 20.0;
        
        out << "<?xml version=\"1.0\" standalone=\"no\"?>\n";
        out << "<svg width=\"" << w << "\" height=\"" << h << "\" "
            << "viewBox=\"" << (bounds.xMin/20.0) << " " << (bounds.yMin/20.0) << " " 
            << w << " " << h << "\" "
            << "xmlns=\"http://www.w3.org/2000/svg\">\n";
            
        if (!defsContent.str().empty()) {
            out << "<defs>\n" << defsContent.str() << "</defs>\n";
        }
        
        out << svgContent.str();
        out << "</svg>\n";
        
        return true;
    }
};

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cout << "Usage: " << argv[0] << " <shape.dat> <version 1-4> <output.svg>" << std::endl;
        return 1;
    }
    
    int version = std::stoi(argv[2]);
    ShapeRenderer renderer(version);
    
    if (renderer.loadShape(argv[1])) {
        if (renderer.renderToSVG(argv[3])) {
            std::cout << "Success!" << std::endl;
            return 0;
        }
    }
    
    std::cout << "Failed." << std::endl;
    return 1;
}