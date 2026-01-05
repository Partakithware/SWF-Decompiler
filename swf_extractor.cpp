#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <zlib.h>
#include <map>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>

// SWF Tag Types
enum TagType {
    TAG_END = 0,
    TAG_SHOW_FRAME = 1,
    TAG_DEFINE_SHAPE = 2,
    TAG_PLACE_OBJECT = 4,
    TAG_REMOVE_OBJECT = 5,
    TAG_DEFINE_BITS = 6,
    TAG_DEFINE_BUTTON = 7,
    TAG_JPEG_TABLES = 8,
    TAG_DEFINE_BITS_JPEG2 = 21,
    TAG_DEFINE_BITS_JPEG3 = 35,
    TAG_DEFINE_BITS_LOSSLESS = 20,
    TAG_DEFINE_BITS_LOSSLESS2 = 36,
    TAG_DEFINE_BITS_JPEG4 = 90,
    TAG_DO_ACTION = 12,
    TAG_DO_ABC = 82,
    TAG_PLACE_OBJECT2 = 26,
    TAG_PLACE_OBJECT3 = 70,
    TAG_REMOVE_OBJECT2 = 28,
    TAG_DEFINE_SHAPE2 = 22,
    TAG_DEFINE_SHAPE3 = 32,
    TAG_DEFINE_SHAPE4 = 83,
    TAG_DEFINE_SPRITE = 39,
    TAG_FILE_ATTRIBUTES = 69,
    TAG_DEFINE_FONT = 10,
    TAG_DEFINE_FONT2 = 48,
    TAG_DEFINE_FONT3 = 75,
    TAG_DEFINE_TEXT = 11,
    TAG_DEFINE_TEXT2 = 33,
    TAG_DEFINE_EDIT_TEXT = 37,
    TAG_DEFINE_SOUND = 14,
    TAG_DEFINE_BINARY_DATA = 87,
    TAG_SYMBOL_CLASS = 76,
    TAG_DEFINE_MORPH_SHAPE = 46,
    TAG_DEFINE_MORPH_SHAPE2 = 84
};

struct Matrix {
    double a, b, c, d, tx, ty;
    Matrix() : a(1), b(0), c(0), d(1), tx(0), ty(0) {}
};

struct ColorTransform {
    int rMult, gMult, bMult, aMult;
    int rAdd, gAdd, bAdd, aAdd;
    ColorTransform() : rMult(256), gMult(256), bMult(256), aMult(256),
                       rAdd(0), gAdd(0), bAdd(0), aAdd(0) {}
};

struct DisplayObject {
    uint16_t characterId;
    uint16_t depth;
    Matrix matrix;
    ColorTransform colorTransform;
    std::string name;
};

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
            return val | (~0u << numBits);
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
};

class SWFExtractor {
    std::vector<uint8_t> data;
    std::string outputDir;
    int currentFrame;
    int globalFrame;
    std::map<int, std::string> characterMap;
    std::map<int, std::string> characterTypes;
    std::map<uint16_t, DisplayObject> displayList;
    std::vector<uint8_t> jpegTables;
    
    void createDirectory(const std::string& path) {
        #ifdef _WIN32
        _mkdir(path.c_str());
        #else
        mkdir(path.c_str(), 0755);
        #endif
    }
    
    uint32_t readU32(size_t& pos) {
        if (pos + 4 > data.size()) return 0;
        uint32_t val = data[pos] | (data[pos+1] << 8) | (data[pos+2] << 16) | (data[pos+3] << 24);
        pos += 4;
        return val;
    }
    
    uint16_t readU16(size_t& pos) {
        if (pos + 2 > data.size()) return 0;
        uint16_t val = data[pos] | (data[pos+1] << 8);
        pos += 2;
        return val;
    }
    
    uint8_t readU8(size_t& pos) {
        if (pos >= data.size()) return 0;
        return data[pos++];
    }
    
    std::string readString(size_t& pos) {
        std::string result;
        while (pos < data.size() && data[pos] != 0) {
            result += (char)data[pos++];
        }
        if (pos < data.size()) pos++;
        return result;
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
    
    ColorTransform readColorTransform(BitReader& br, bool hasAlpha) {
        ColorTransform ct;
        bool hasAdd = br.readBits(1);
        bool hasMult = br.readBits(1);
        int nBits = br.readBits(4);
        
        if (hasMult) {
            ct.rMult = br.readSignedBits(nBits);
            ct.gMult = br.readSignedBits(nBits);
            ct.bMult = br.readSignedBits(nBits);
            if (hasAlpha) ct.aMult = br.readSignedBits(nBits);
        }
        if (hasAdd) {
            ct.rAdd = br.readSignedBits(nBits);
            ct.gAdd = br.readSignedBits(nBits);
            ct.bAdd = br.readSignedBits(nBits);
            if (hasAlpha) ct.aAdd = br.readSignedBits(nBits);
        }
        return ct;
    }
    
    void extractShape(size_t tagStart, size_t tagLength, int characterId, int shapeVersion) {
        std::stringstream filename;
        filename << outputDir << "/shape_" << characterId << ".dat";
        
        std::ofstream out(filename.str(), std::ios::binary);
        if (out.is_open() && tagStart + tagLength <= data.size()) {
            out.write((const char*)&data[tagStart], tagLength);
            out.close();
            
            std::stringstream infofile;
            infofile << outputDir << "/shape_" << characterId << "_info.txt";
            std::ofstream info(infofile.str());
            info << "Shape ID: " << characterId << "\n";
            info << "Version: " << shapeVersion << "\n";
            info << "Data size: " << tagLength << " bytes\n";
            info.close();
            
            std::cout << "Extracted shape: " << filename.str() << " (ID: " << characterId << ", v" << shapeVersion << ")" << std::endl;
            characterMap[characterId] = filename.str();
            characterTypes[characterId] = "shape";
        }
    }
    
    void extractJPEG(const uint8_t* imgData, size_t imgSize, int characterId, bool hasTables) {
        std::stringstream filename;
        filename << outputDir << "/image_" << characterId << ".jpg";
        
        std::ofstream out(filename.str(), std::ios::binary);
        if (out.is_open()) {
            if (hasTables && jpegTables.size() > 0 && imgSize > 2 && !(imgData[0] == 0xFF && imgData[1] == 0xD8)) {
                size_t tableSize = jpegTables.size();
                if (tableSize >= 2 && jpegTables[tableSize-2] == 0xFF && jpegTables[tableSize-1] == 0xD9) {
                    out.write((const char*)jpegTables.data(), tableSize - 2);
                } else {
                    out.write((const char*)jpegTables.data(), tableSize);
                }
                if (imgData[0] == 0xFF && imgData[1] == 0xD8) {
                    out.write((const char*)(imgData + 2), imgSize - 2);
                } else {
                    out.write((const char*)imgData, imgSize);
                }
            } else {
                out.write((const char*)imgData, imgSize);
            }
            out.close();
            std::cout << "Extracted JPEG: " << filename.str() << " (ID: " << characterId << ")" << std::endl;
            characterMap[characterId] = filename.str();
            characterTypes[characterId] = "image";
        }
    }
    
    void extractPNG(const uint8_t* imgData, size_t width, size_t height, int format, int characterId, bool hasAlpha) {
        std::stringstream filename;
        filename << outputDir << "/image_" << characterId << ".raw";
        
        int bpp = hasAlpha ? 4 : 3;
        std::ofstream out(filename.str(), std::ios::binary);
        if (out.is_open()) {
            out.write((const char*)imgData, width * height * bpp);
            out.close();
            
            std::stringstream info;
            info << outputDir << "/image_" << characterId << "_info.txt";
            std::ofstream infoFile(info.str());
            infoFile << "Width: " << width << "\n";
            infoFile << "Height: " << height << "\n";
            infoFile << "BPP: " << bpp << "\n";
            infoFile << "Format: " << (hasAlpha ? "RGBA" : "RGB") << "\n";
            infoFile.close();
            
            std::cout << "Extracted bitmap: " << filename.str() << " (" << width << "x" << height << ")" << std::endl;
            characterMap[characterId] = filename.str();
            characterTypes[characterId] = "image";
        }
    }
    
    void extractBinaryData(const uint8_t* binData, size_t binSize, int characterId) {
        std::stringstream filename;
        filename << outputDir << "/binary_" << characterId << ".bin";
        
        std::ofstream out(filename.str(), std::ios::binary);
        if (out.is_open()) {
            out.write((const char*)binData, binSize);
            out.close();
            std::cout << "Extracted binary data: " << filename.str() << " (" << binSize << " bytes)" << std::endl;
            characterMap[characterId] = filename.str();
            characterTypes[characterId] = "binary";
        }
    }
    
    void extractSound(const uint8_t* soundData, size_t soundSize, int characterId, int format) {
        std::stringstream filename;
        const char* ext = ".raw";
        if (format == 2) ext = ".mp3";
        else if (format == 3) ext = ".raw";
        
        filename << outputDir << "/sound_" << characterId << ext;
        
        std::ofstream out(filename.str(), std::ios::binary);
        if (out.is_open()) {
            out.write((const char*)soundData, soundSize);
            out.close();
            std::cout << "Extracted sound: " << filename.str() << " (format=" << format << ")" << std::endl;
            characterMap[characterId] = filename.str();
            characterTypes[characterId] = "sound";
        }
    }
    
    void extractActionScript(const uint8_t* scriptData, size_t scriptSize, int frameNum, int scriptNum, const std::string& context = "") {
        std::stringstream filename;
        if (context.empty()) {
            filename << outputDir << "/frame_" << std::setw(4) << std::setfill('0') << frameNum 
                     << "_action_" << scriptNum << ".as";
        } else {
            filename << outputDir << "/" << context << "_action_" << scriptNum << ".as";
        }
        
        std::ofstream out(filename.str(), std::ios::binary);
        if (out.is_open()) {
            out.write((const char*)scriptData, scriptSize);
            out.close();
            
            std::stringstream hexfile;
            hexfile << filename.str() << ".hex";
            std::ofstream hexout(hexfile.str());
            for (size_t i = 0; i < scriptSize; i++) {
                hexout << std::hex << std::setw(2) << std::setfill('0') << (int)scriptData[i] << " ";
                if ((i + 1) % 16 == 0) hexout << "\n";
            }
            hexout.close();
            
            std::cout << "Extracted ActionScript: " << filename.str() << " (" << scriptSize << " bytes)" << std::endl;
        }
    }
    
    void saveFrameState(int frameNum) {
        std::stringstream filename;
        filename << outputDir << "/frame_" << std::setw(4) << std::setfill('0') << frameNum << "_display.txt";
        
        std::ofstream out(filename.str());
        if (out.is_open()) {
            out << "=== FRAME " << frameNum << " DISPLAY LIST ===" << "\n\n";
            
            for (auto& pair : displayList) {
                DisplayObject& obj = pair.second;
                out << "Depth: " << obj.depth << "\n";
                out << "  Character ID: " << obj.characterId << "\n";
                
                if (characterTypes.count(obj.characterId)) {
                    out << "  Type: " << characterTypes[obj.characterId] << "\n";
                }
                if (characterMap.count(obj.characterId)) {
                    out << "  File: " << characterMap[obj.characterId] << "\n";
                }
                
                out << "  Matrix: [" << obj.matrix.a << ", " << obj.matrix.b << ", "
                    << obj.matrix.c << ", " << obj.matrix.d << ", "
                    << obj.matrix.tx << ", " << obj.matrix.ty << "]\n";
                
                if (!obj.name.empty()) {
                    out << "  Name: " << obj.name << "\n";
                }
                out << "\n";
            }
            out.close();
            std::cout << "Saved frame state: " << filename.str() << " (" << displayList.size() << " objects)" << std::endl;
        }
    }
    
    void processSprite(uint16_t spriteId, size_t& pos, size_t endPos) {
        std::cout << "Processing sprite " << spriteId << " contents..." << std::endl;
        int spriteFrame = 0;
        int actionCount = 0;
        
        std::stringstream spriteContext;
        spriteContext << "sprite_" << spriteId;
        
        std::stringstream metafile;
        metafile << outputDir << "/sprite_" << spriteId << "_info.txt";
        std::ofstream meta(metafile.str());
        meta << "Sprite ID: " << spriteId << "\n";
        meta << "Contains:\n";
        
        while (pos < endPos && pos < data.size()) {
            uint16_t tagCodeAndLength = readU16(pos);
            uint16_t tagType = tagCodeAndLength >> 6;
            uint32_t tagLength = tagCodeAndLength & 0x3F;
            
            if (tagLength == 0x3F) {
                tagLength = readU32(pos);
            }
            
            if (tagType == TAG_END) break;
            
            size_t tagStart = pos;
            
            switch (tagType) {
                case TAG_SHOW_FRAME:
                    spriteFrame++;
                    meta << "  Frame " << spriteFrame << "\n";
                    break;
                    
                case TAG_DO_ACTION: {
                    if (pos + tagLength <= data.size()) {
                        std::stringstream ctx;
                        ctx << spriteContext.str() << "_frame_" << spriteFrame;
                        extractActionScript(&data[pos], tagLength, spriteFrame, actionCount++, ctx.str());
                        meta << "    Action script\n";
                    }
                    break;
                }
                
                default:
                    processTag(tagType, tagLength, pos);
                    pos = tagStart + tagLength;
                    break;
            }
            
            pos = tagStart + tagLength;
        }
        
        meta.close();
        characterMap[spriteId] = metafile.str();
        characterTypes[spriteId] = "sprite";
    }
    
    void processTag(uint16_t tagType, uint32_t tagLength, size_t& pos) {
        size_t tagStart = pos;
        
        switch (tagType) {
            case TAG_SHOW_FRAME: {
                currentFrame++;
                globalFrame++;
                std::cout << "\n=== Frame " << currentFrame << " ===" << std::endl;
                saveFrameState(currentFrame);
                break;
            }
            
            case TAG_JPEG_TABLES: {
                jpegTables.clear();
                if (pos + tagLength <= data.size()) {
                    jpegTables.assign(data.begin() + pos, data.begin() + pos + tagLength);
                    std::cout << "Loaded JPEG tables (" << tagLength << " bytes)" << std::endl;
                }
                pos += tagLength;
                break;
            }
            
            case TAG_DEFINE_SHAPE:
            case TAG_DEFINE_SHAPE2:
            case TAG_DEFINE_SHAPE3:
            case TAG_DEFINE_SHAPE4: {
                uint16_t characterId = readU16(pos);
                int shapeVersion = (tagType == TAG_DEFINE_SHAPE) ? 1 :
                                  (tagType == TAG_DEFINE_SHAPE2) ? 2 :
                                  (tagType == TAG_DEFINE_SHAPE3) ? 3 : 4;
                pos = tagStart;
                extractShape(pos, tagLength, characterId, shapeVersion);
                pos = tagStart + tagLength;
                break;
            }
            
            case TAG_DEFINE_MORPH_SHAPE:
            case TAG_DEFINE_MORPH_SHAPE2: {
                uint16_t characterId = readU16(pos);
                std::stringstream filename;
                filename << outputDir << "/morph_shape_" << characterId << ".dat";
                std::ofstream out(filename.str(), std::ios::binary);
                if (out.is_open()) {
                    out.write((const char*)&data[tagStart], tagLength);
                    out.close();
                    std::cout << "Extracted morph shape: " << filename.str() << std::endl;
                    characterMap[characterId] = filename.str();
                    characterTypes[characterId] = "morph_shape";
                }
                pos = tagStart + tagLength;
                break;
            }
            
            case TAG_DEFINE_BITS: {
                uint16_t characterId = readU16(pos);
                size_t imgSize = tagLength - 2;
                if (pos + imgSize <= data.size()) {
                    extractJPEG(&data[pos], imgSize, characterId, true);
                    pos += imgSize;
                }
                break;
            }
            
            case TAG_DEFINE_BITS_JPEG2: {
                uint16_t characterId = readU16(pos);
                size_t imgSize = tagLength - 2;
                if (pos + imgSize <= data.size()) {
                    extractJPEG(&data[pos], imgSize, characterId, false);
                    pos += imgSize;
                }
                break;
            }
            
            case TAG_DEFINE_BITS_JPEG3: 
            case TAG_DEFINE_BITS_JPEG4: {
                uint16_t characterId = readU16(pos);
                uint32_t alphaDataOffset = readU32(pos);
                size_t imgSize = alphaDataOffset;
                if (pos + imgSize <= data.size()) {
                    extractJPEG(&data[pos], imgSize, characterId, false);
                    pos = tagStart + tagLength;
                }
                break;
            }
            
            case TAG_DEFINE_BITS_LOSSLESS:
            case TAG_DEFINE_BITS_LOSSLESS2: {
                uint16_t characterId = readU16(pos);
                uint8_t format = readU8(pos);
                uint16_t width = readU16(pos);
                uint16_t height = readU16(pos);
                
                size_t dataSize = tagLength - 7;
                uint8_t colorTableSize = 0;
                if (format == 3) {
                    colorTableSize = readU8(pos);
                    dataSize--;
                }
                
                std::vector<uint8_t> decompressed;
                if (pos + dataSize <= data.size()) {
                    size_t estimatedSize = width * height * 4 + (colorTableSize + 1) * 4;
                    decompressed.resize(estimatedSize);
                    uLongf destLen = decompressed.size();
                    int result = uncompress(decompressed.data(), &destLen, &data[pos], dataSize);
                    if (result == Z_OK) {
                        decompressed.resize(destLen);
                        extractPNG(decompressed.data(), width, height, format, 
                                  characterId, tagType == TAG_DEFINE_BITS_LOSSLESS2);
                    } else {
                        std::cerr << "Failed to decompress bitmap " << characterId << std::endl;
                    }
                }
                pos = tagStart + tagLength;
                break;
            }
            
            case TAG_DEFINE_BINARY_DATA: {
                uint16_t characterId = readU16(pos);
                uint32_t reserved = readU32(pos);
                size_t binSize = tagLength - 6;
                if (pos + binSize <= data.size()) {
                    extractBinaryData(&data[pos], binSize, characterId);
                    pos += binSize;
                }
                break;
            }
            
            case TAG_DEFINE_SOUND: {
                uint16_t characterId = readU16(pos);
                uint8_t soundFormat = (readU8(pos) >> 4) & 0x0F;
                pos--;
                uint8_t flags = readU8(pos);
                uint32_t sampleCount = readU32(pos);
                size_t soundSize = tagLength - 7;
                if (pos + soundSize <= data.size()) {
                    extractSound(&data[pos], soundSize, characterId, soundFormat);
                    pos += soundSize;
                }
                break;
            }
            
            case TAG_DO_ACTION: {
                static int actionCount = 0;
                if (pos + tagLength <= data.size()) {
                    extractActionScript(&data[pos], tagLength, currentFrame, actionCount++);
                    pos += tagLength;
                }
                break;
            }
            
            case TAG_DO_ABC: {
                static int abcCount = 0;
                if (pos + tagLength <= data.size()) {
                    std::stringstream filename;
                    filename << outputDir << "/abc_" << abcCount++ << ".abc";
                    std::ofstream out(filename.str(), std::ios::binary);
                    if (out.is_open()) {
                        out.write((const char*)&data[pos], tagLength);
                        out.close();
                        std::cout << "Extracted ABC bytecode: " << filename.str() << std::endl;
                    }
                    pos += tagLength;
                }
                break;
            }
            
            case TAG_SYMBOL_CLASS: {
                uint16_t numSymbols = readU16(pos);
                std::cout << "SymbolClass with " << numSymbols << " symbols:" << std::endl;
                
                std::stringstream filename;
                filename << outputDir << "/symbol_class.txt";
                std::ofstream out(filename.str());
                
                for (int i = 0; i < numSymbols; i++) {
                    uint16_t tagId = readU16(pos);
                    std::string name = readString(pos);
                    std::cout << "  Symbol " << tagId << " = " << name << std::endl;
                    out << tagId << "\t" << name << "\n";
                }
                out.close();
                break;
            }
            
            case TAG_PLACE_OBJECT: {
                uint16_t characterId = readU16(pos);
                uint16_t depth = readU16(pos);
                
                BitReader br(&data[pos], data.size() - pos);
                Matrix matrix = readMatrix(br);
                br.alignByte();
                
                DisplayObject obj;
                obj.characterId = characterId;
                obj.depth = depth;
                obj.matrix = matrix;
                displayList[depth] = obj;
                
                std::cout << "PlaceObject: char=" << characterId << ", depth=" << depth << std::endl;
                pos = tagStart + tagLength;
                break;
            }
            
            case TAG_PLACE_OBJECT2:
            case TAG_PLACE_OBJECT3: {
                uint8_t flags = readU8(pos);
                uint16_t depth = readU16(pos);
                
                DisplayObject obj;
                if (displayList.count(depth)) {
                    obj = displayList[depth];
                }
                obj.depth = depth;
                
                if (flags & 0x02) {
                    obj.characterId = readU16(pos);
                }
                
                if (flags & 0x04) {
                    BitReader br(&data[pos], data.size() - pos);
                    obj.matrix = readMatrix(br);
                    br.alignByte();
                    pos = tagStart + (br.getBytePos() - (tagStart - pos));
                }
                
                if (flags & 0x08) {
                    BitReader br(&data[pos], data.size() - pos);
                    obj.colorTransform = readColorTransform(br, tagType == TAG_PLACE_OBJECT3);
                    br.alignByte();
                    pos = tagStart + (br.getBytePos() - (tagStart - pos));
                }
                
                if (flags & 0x20) {
                    obj.name = readString(pos);
                }
                
                displayList[depth] = obj;
                
                std::cout << "PlaceObject" << (tagType == TAG_PLACE_OBJECT3 ? "3" : "2") 
                         << ": char=" << obj.characterId << ", depth=" << depth;
                if (!obj.name.empty()) std::cout << ", name=" << obj.name;
                std::cout << std::endl;
                
                pos = tagStart + tagLength;
                break;
            }
            
            case TAG_REMOVE_OBJECT: {
                uint16_t characterId = readU16(pos);
                uint16_t depth = readU16(pos);
                displayList.erase(depth);
                std::cout << "RemoveObject: char=" << characterId << ", depth=" << depth << std::endl;
                break;
            }
            
            case TAG_REMOVE_OBJECT2: {
                uint16_t depth = readU16(pos);
                displayList.erase(depth);
                std::cout << "RemoveObject2: depth=" << depth << std::endl;
                break;
            }
            
            case TAG_DEFINE_SPRITE: {
                uint16_t spriteId = readU16(pos);
                uint16_t frameCount = readU16(pos);
                std::cout << "\nSprite " << spriteId << " with " << frameCount << " frames" << std::endl;
                
                size_t spriteEnd = tagStart + tagLength;
                processSprite(spriteId, pos, spriteEnd);
                pos = spriteEnd;
                break;
            }
            
            default:
                pos = tagStart + tagLength;
                break;
        }
    }
    
public:
    SWFExtractor(const std::string& outDir) : outputDir(outDir), currentFrame(0), globalFrame(0) {
        createDirectory(outputDir);
    }
    
    bool loadSWF(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open file: " << filename << std::endl;
            return false;
        }
        
        char signature[3];
        file.read(signature, 3);
        
        uint8_t version;
        file.read((char*)&version, 1);
        
        uint32_t fileLength;
        file.read((char*)&fileLength, 4);
        
        std::cout << "SWF Version: " << (int)version << std::endl;
        std::cout << "File Length: " << fileLength << std::endl;
        
        std::vector<uint8_t> fileData;
        fileData.resize(fileLength - 8);
        file.read((char*)fileData.data(), fileData.size());
        file.close();
        
        if (signature[0] == 'C') {
            std::cout << "Decompressing SWF..." << std::endl;
            data.resize(fileLength - 8);
            uLongf destLen = data.size();
            int result = uncompress(data.data(), &destLen, fileData.data(), fileData.size());
            if (result != Z_OK) {
                std::cerr << "Decompression failed!" << std::endl;
                return false;
            }
            data.resize(destLen);
        } else if (signature[0] == 'F') {
            data = fileData;
        } else {
            std::cerr << "Unknown SWF format!" << std::endl;
            return false;
        }
        
        return true;
    }
    
    void extract() {
        size_t pos = 0;
        
        BitReader br(data.data(), data.size());
        int nBits = br.readBits(5);
        br.readSignedBits(nBits);
        br.readSignedBits(nBits);
        br.readSignedBits(nBits);
        br.readSignedBits(nBits);
        br.alignByte();
        pos = br.getBytePos();
        
        uint16_t frameRate = readU16(pos);
        uint16_t frameCount = readU16(pos);
        
        std::cout << "Frame Rate: " << (frameRate / 256.0) << " fps" << std::endl;
        std::cout << "Frame Count: " << frameCount << std::endl;
        std::cout << "\n=== Processing Tags ===" << std::endl;
        
        while (pos < data.size()) {
            uint16_t tagCodeAndLength = readU16(pos);
            uint16_t tagType = tagCodeAndLength >> 6;
            uint32_t tagLength = tagCodeAndLength & 0x3F;
            
            if (tagLength == 0x3F) {
                tagLength = readU32(pos);
            }
            
            if (tagType == TAG_END) break;
            
            processTag(tagType, tagLength, pos);
        }
        
        std::cout << "\n=== Extraction Summary ===" << std::endl;
        std::cout << "Total frames: " << currentFrame << std::endl;
        std::cout << "Total assets extracted: " << characterMap.size() << std::endl;
        std::cout << "\nAsset breakdown:" << std::endl;
        
        std::map<std::string, int> typeCounts;
        for (auto& pair : characterTypes) {
            typeCounts[pair.second]++;
        }
        
        for (auto& pair : typeCounts) {
            std::cout << "  " << pair.first << ": " << pair.second << std::endl;
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "Usage: " << argv[0] << " <input.swf> <output_directory>" << std::endl;
        return 1;
    }
    
    SWFExtractor extractor(argv[2]);
    
    if (!extractor.loadSWF(argv[1])) {
        return 1;
    }
    
    extractor.extract();
    
    return 0;
}