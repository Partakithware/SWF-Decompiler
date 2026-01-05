#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <sstream>
#include <stack>
#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;

namespace fs = std::filesystem;

// --- Safety Helpers ---

// Prevents infinite loops on corrupt files
u32 readU30(std::istream& in) {
    u32 v = 0;
    int shift = 0;
    while (in.good()) {
        char c;
        if (!in.get(c)) break; 
        
        u8 b = (u8)c;
        v |= (b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
        
        shift += 7;
        if (shift > 35) throw std::runtime_error("Integer overflow in readU30");
    }
    return 0;
}

// Prevents freezing the PC by trying to allocate 4GB of RAM
template <typename T>
void safeResize(std::vector<T>& vec, u32 count, const std::string& context) {
    if (count > 2000000) { // Limit to 2 million items
        throw std::runtime_error("File corruption detected: " + context + " count too high (" + std::to_string(count) + ")");
    }
    vec.resize(count);
}
// ----------------------

u32 readU30FromBytes(const std::vector<u8>& data, size_t& pos) {
    u32 v = 0;
    int shift = 0;
    while (pos < data.size()) {
        u8 b = data[pos++];
        v |= (b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}

i32 readS24(const std::vector<u8>& data, size_t& pos) {
    if (pos + 3 > data.size()) return 0;
    i32 v = data[pos] | (data[pos+1] << 8) | (data[pos+2] << 16);
    if (v & 0x800000) v |= 0xFF000000;
    pos += 3;
    return v;
}

std::string readString(std::istream& in) {
    u32 len = readU30(in);
    if (len > 1000000) throw std::runtime_error("String length too big");
    std::string s(len, '\0');
    in.read(s.data(), len);
    return s;
}

struct ConstantPool {
    std::vector<i32> ints;
    std::vector<u32> uints;
    std::vector<double> doubles;
    std::vector<std::string> strings;
};

struct Multiname {
    u8 kind = 0;
    u32 nsIndex = 0;
    u32 nameIndex = 0;
};

struct MethodInfo {
    u32 name = 0;
    u32 paramCount = 0;
};

struct MethodBody {
    u32 method = 0;
    u32 maxStack = 0;
    u32 localCount = 0;
    std::vector<u8> code;
};

struct Trait {
    u32 name = 0;
    u8 kind = 0;
    u32 methodIndex = 0;
    u32 classIndex = 0;
};


struct Script {
    u32 init = 0;
    std::vector<Trait> traits;
};

struct InstanceInfo {
    u32 name;
    u32 superName;
    u32 iinit;
    std::vector<Trait> traits;
};

struct ClassInfo {
    u32 cinit;
    std::vector<Trait> traits;
};

struct ClassDef {
    InstanceInfo instance;
    ClassInfo statics;
};

struct Namespace {
    u8 kind;
    u32 name; // string index
};

struct StackValue {
    enum class Type { Literal, Object, Unknown } type;
    std::string value;

    StackValue(Type t, const std::string& v) : type(t), value(v) {}
};

struct ABC {
    ConstantPool cp;
    std::vector<Multiname> multinames;
    std::vector<MethodInfo> methods;
    std::vector<MethodBody> bodies;
    std::vector<Script> scripts;
    std::vector<ClassDef> classes;
    std::vector<Namespace> namespaces;
};


class ABCParser {
public:
    explicit ABCParser(std::istream& in) : in(in) {}

ABC parse() {
        ABC abc;
        readVersion();
        parseConstantPool(abc);
        
        std::cout << "Checkpoint 1: Methods at offset " << in.tellg() << std::endl;
        parseMethods(abc);
        
        std::cout << "Checkpoint 2: Metadata at offset " << in.tellg() << std::endl;
        skipMetadata();
        
        std::cout << "Checkpoint 3: Classes at offset " << in.tellg() << std::endl;
        //skipClasses();
        parseClasses(abc);
        
        std::cout << "Checkpoint 4: Scripts at offset " << in.tellg() << std::endl;
        parseScripts(abc);
        
        std::cout << "Checkpoint 5: Bodies at offset " << in.tellg() << std::endl;
        parseMethodBodies(abc);
        
        return abc;
    }


private:
    std::istream& in;

    void readVersion() {
        u16 minor, major;
        in.read((char*)&minor, 2);
        in.read((char*)&major, 2);
        // ABC files are little-endian. 10 00 = 16, 2E 00 = 46.
        std::cout << "ABC Version: " << major << "." << minor << std::endl;
    }

    /*std::string getPackage(u32 multinameIndex) const {
        if (multinameIndex == 0 || multinameIndex >= abc.multinames.size())
            return "";

        const Multiname& mn = abc.multinames[multinameIndex];
        if (mn.nsIndex == 0 || mn.nsIndex >= abc.namespaces.size())
            return "";

        const Namespace& ns = abc.namespaces[mn.nsIndex];
        if (ns.name == 0 || ns.name >= abc.cp.strings.size())
            return "";

        return abc.cp.strings[ns.name];
    }*/

    void parseConstantPool(ABC& abc) {
        u32 ic = readU30(in);
        safeResize(abc.cp.ints, ic, "Integer Pool");
        for (u32 i = 1; i < ic; i++) abc.cp.ints[i] = readU30(in);

        u32 uc = readU30(in);
        safeResize(abc.cp.uints, uc, "UInt Pool");
        for (u32 i = 1; i < uc; i++) abc.cp.uints[i] = readU30(in);

        u32 dc = readU30(in);
        safeResize(abc.cp.doubles, dc, "Double Pool");
        for (u32 i = 1; i < dc; i++) {
            double d;
            in.read((char*)&d, 8);
            abc.cp.doubles[i] = d;
        }

        u32 sc = readU30(in);
        safeResize(abc.cp.strings, sc, "String Pool");
        for (u32 i = 1; i < sc; i++) abc.cp.strings[i] = readString(in);

        u32 nsc = readU30(in);
        safeResize(abc.namespaces, nsc, "Namespaces");
        for (u32 i = 1; i < nsc; i++) {
            abc.namespaces[i].kind = in.get();
            abc.namespaces[i].name = readU30(in);
        }
        
        u32 nssc = readU30(in);
        for(u32 i=1; i<nssc; i++) {
            u32 cnt = readU30(in);
            for(u32 j=0; j<cnt; j++) readU30(in);
        }

        u32 mc = readU30(in);
        std::cout << "  Reading " << mc << " multinames..." << std::endl;
        safeResize(abc.multinames, mc, "Multiname Pool");
        for (u32 i = 1; i < mc; i++) {
            u8 kind;
            in.read((char*)&kind, 1);
            abc.multinames[i].kind = kind;
            
            
            switch (kind) {
                case 0x07:
                case 0x0D:
                    abc.multinames[i].nsIndex   = readU30(in);
                    abc.multinames[i].nameIndex = readU30(in);
                    break;
                case 0x0F: // RTQName
                case 0x10: // RTQNameA
                    abc.multinames[i].nameIndex = readU30(in); // name
                    break;
                case 0x11: // RTQNameL
                case 0x12: // RTQNameLA
                    break; 
                case 0x09: // Multiname
                case 0x0E: // MultinameA
                    abc.multinames[i].nameIndex = readU30(in); // name
                    readU30(in); // ns_set
                    break;
                case 0x1B: // MultinameL
                case 0x1C: // MultinameLA
                    readU30(in); // ns_set
                    break;
                case 0x1D: { // Generic (Braces added to fix compiler error)
                    abc.multinames[i].nameIndex = readU30(in); // name
                    u32 gcount = readU30(in);
                    for(u32 j=0; j<gcount; j++) readU30(in);
                    break;
                }
                default:
                    throw std::runtime_error("Unknown Multiname Kind: " + std::to_string((int)kind));
            }
        }
    }

    void parseMethods(ABC& abc) {
            u32 count = readU30(in);
            std::cout << "  Methods count: " << count << std::endl;
            safeResize(abc.methods, count, "Methods");
            for (u32 i = 0; i < count; i++) {
                u32 paramCount = readU30(in);
                abc.methods[i].paramCount = paramCount;
                readU30(in); // returnType
                for (u32 j = 0; j < paramCount; j++) readU30(in); // paramTypes
                abc.methods[i].name = readU30(in); // name
                
                u8 flags; 
                in.read((char*)&flags, 1);
                
                if (flags & 0x08) { // HAS_OPTIONAL
                    u32 optCount = readU30(in);
                    for (u32 j = 0; j < optCount; j++) {
                        readU30(in); // value index
                        in.get();    // kind
                    }
                }
                if (flags & 0x80) { // HAS_PARAM_NAMES
                    for (u32 j = 0; j < paramCount; j++) {
                        readU30(in); // name index
                    }
                }
            }
        }

    void skipMetadata() {
        u32 c = readU30(in);
        for (u32 i = 0; i < c; i++) {
            readU30(in);
            u32 kv = readU30(in);
            for (u32 j = 0; j < kv * 2; j++) readU30(in);
        }
    }

void skipClasses() {
    u32 count = readU30(in);
    if (count == 0) return;
    
    // Safety check to prevent the core dump
    if (count > 100000) throw std::runtime_error("Corrupt Class count: " + std::to_string(count));

    // 1. Instance Info
    for (u32 i = 0; i < count; i++) {
        readU30(in); // name index
        readU30(in); // super_name index
        u8 flags = in.get(); 
        
        if (flags & 0x08) readU30(in); // ProtectedNs
        if (flags & 0x10) readU30(in); // NsSet
        if (flags & 0x20) readU30(in); // Stub/Lazy
        
        u32 interfaceCount = readU30(in);
        // Safety check for interfaces
        if (interfaceCount > 1000) throw std::runtime_error("Corrupt interface count");
        for (u32 j = 0; j < interfaceCount; j++) {
            readU30(in); 
        }
        
        readU30(in); // iinit
        skipTraits(); // instance traits
    }
    
    // 2. Class Info
    for (u32 i = 0; i < count; i++) {
        readU30(in); // cinit
        skipTraits(); // class traits
    }
}

void parseClasses(ABC& abc) {
    u32 count = readU30(in);
    safeResize(abc.classes, count, "Classes");

    // Instance info
    for (u32 i = 0; i < count; i++) {
        InstanceInfo& inst = abc.classes[i].instance;
        inst.name = readU30(in);
        inst.superName = readU30(in);
        u8 flags = in.get();

        if (flags & 0x08) readU30(in);
        if (flags & 0x10) readU30(in);
        if (flags & 0x20) readU30(in);

        u32 ifaceCount = readU30(in);
        for (u32 j = 0; j < ifaceCount; j++) readU30(in);

        inst.iinit = readU30(in);

        u32 tc = readU30(in);
        for (u32 j = 0; j < tc; j++) {
            Trait t;
            t.name = readU30(in);
            in.read((char*)&t.kind, 1);
            readTraitData(t.kind, t);
            inst.traits.push_back(t);
        }
    }

    // Class (static) info
    for (u32 i = 0; i < count; i++) {
        ClassInfo& cls = abc.classes[i].statics;
        cls.cinit = readU30(in);

        u32 tc = readU30(in);
        for (u32 j = 0; j < tc; j++) {
            Trait t;
            t.name = readU30(in);
            in.read((char*)&t.kind, 1);
            readTraitData(t.kind, t);
            cls.traits.push_back(t);
        }
    }
}



    void parseScripts(ABC& abc) {
        u32 count = readU30(in);
        safeResize(abc.scripts, count, "Scripts");
        for (u32 i = 0; i < count; i++) {
            Script& s = abc.scripts[i];
            s.init = readU30(in);
            u32 tc = readU30(in);
            for (u32 j = 0; j < tc; j++) {
            Trait t;
            t.name = readU30(in);
            in.read((char*)&t.kind, 1);
            readTraitData(t.kind, t);
            s.traits.push_back(t);
            }
        }
    }

    void parseMethodBodies(ABC& abc) {
        u32 count = readU30(in);
        safeResize(abc.bodies, count, "MethodBodies");
        for (u32 i = 0; i < count; i++) {
            MethodBody& b = abc.bodies[i];
            b.method = readU30(in);
            b.maxStack = readU30(in);
            b.localCount = readU30(in);
            readU30(in); readU30(in);
            u32 len = readU30(in);
            safeResize(b.code, len, "Method Code");
            in.read((char*)b.code.data(), len);
            skipExceptions(); // correct
            skipTraits();     // correct
        }
    }

    void skipExceptions() {
        u32 count = readU30(in);
        for (u32 i = 0; i < count; i++) {
            readU30(in); // from
            readU30(in); // to
            readU30(in); // target
            readU30(in); // exc_type
            readU30(in); // var_name
        }
    }

void readTraitData(u8 kind, Trait& t) {
    const u8 traitKind = kind & 0x0F;

    readU30(in); // slot_id or disp_id

    switch (traitKind) {
        case 0: // Slot
        case 6: // Const
            readU30(in); // type
            if (readU30(in) != 0) in.get();
            break;

        case 1: case 2: case 3:
            t.methodIndex = readU30(in);
            break;

        case 4: // Class
            t.classIndex = readU30(in);
            break;

        case 5: // Function
            readU30(in);
            break;

        default:
            throw std::runtime_error("Unknown trait kind");
    }
}

void skipTraits() {
    u32 traitCount = readU30(in);
    for (u32 i = 0; i < traitCount; i++) {
        readU30(in); // name
        u8 kind = in.get();

        Trait dummy;
        readTraitData(kind, dummy);

        if (kind & 0x40) {
            u32 metadataCount = readU30(in);
            for (u32 m = 0; m < metadataCount; m++)
                readU30(in);
        }
    }
}

}; // END ABCPars

class Decompiler {
    const ABC& abc;
    std::stack<std::string> stack;
    std::vector<std::string> locals;
    std::stringstream output;
    int indent;

    std::string getString(u32 idx) {
        if (idx < abc.cp.strings.size())
            return abc.cp.strings[idx];
        return "";
    }

    void out(const std::string& str) {
        for (int i = 0; i < indent; i++) output << "    ";
        output << str << "\n";
    }

bool isNonSemanticOpcode(uint16_t op) {
    switch (op) {
        // Scope / stack housekeeping
        case 0x19: case 0x20: case 0x21: case 0x26: case 0x27:
        case 0x30: case 0x34: case 0x93: case 0x128: case 0x130:
        case 0x1D: case 0x1E: case 0xD0: case 0xD1: case 0xD2: case 0xD3:
            return true;
        default:
            return false;
    }
}
    
    std::string fixNumericPrefix(const std::string& expr) {
        // Detect leading number followed by dot
        // e.g., "3.hpBar.updateHPBar()" => "hpBar.updateHPBar()"
        size_t dotPos = expr.find('.');
        if (dotPos != std::string::npos) {
            std::string prefix = expr.substr(0, dotPos);
            if (std::all_of(prefix.begin(), prefix.end(), ::isdigit)) {
                return expr.substr(dotPos + 1);
            }
        }
        return expr;
    }

public:
    Decompiler(const ABC& abc) : abc(abc), indent(0) {}
    bool keepOpcodeComments = false; // default
    

    std::string getName(u32 idx) {
        if (idx == 0 || idx >= abc.multinames.size()) return "unknown";
        const auto& mn = abc.multinames[idx];
        if (mn.nameIndex < abc.cp.strings.size())
            return abc.cp.strings[mn.nameIndex];
        return "name" + std::to_string(idx);
    } 
    
    std::string getPackage(u32 multinameIndex) const {
        if (multinameIndex == 0 || multinameIndex >= abc.multinames.size())
            return "";

        const Multiname& mn = abc.multinames[multinameIndex];
        if (mn.nsIndex == 0 || mn.nsIndex >= abc.namespaces.size())
            return "";

        const Namespace& ns = abc.namespaces[mn.nsIndex];
        if (ns.name == 0 || ns.name >= abc.cp.strings.size())
            return "";

        return abc.cp.strings[ns.name];
    }

    std::string decompileMethod(const MethodBody& body) {
        std::unordered_set<size_t> jumpTargets; // track jump destinations
        output.str("");
        output.clear();
        stack = std::stack<std::string>();
        locals.clear();
        locals.resize(body.localCount > 0 ? body.localCount : 4, "undefined");
        
        size_t pc = 0;
        const auto& code = body.code;
        indent = 1;

        while (pc < code.size()) {
            u8 op = code[pc++];

        if (jumpTargets.contains(pc)) {
            if (keepOpcodeComments) {
                output << "label_" << pc << ":\n";
            } else {
                output << "label_" << pc << ":\n"; // label is needed for jumps
            }
        }    
        
        // Skip non-semantic opcodes completely if flag is false
        if (isNonSemanticOpcode(op)) {
            if (keepOpcodeComments) {
                output << "// opcode 0x" << std::hex << int(op) << "\n";
            }
            continue;
        }
            switch (op) {
                case 0x47: // returnvoid
                    out("return;");
                    break;

                case 0x48: // returnvalue
                    if (!stack.empty()) {
                        out("return " + stack.top() + ";");
                        stack.pop();
                    }
                    break;

                case 0x20: stack.push("null"); break;
                case 0x21: stack.push("undefined"); break;
                case 0x26: stack.push("true"); break;
                case 0x27: stack.push("false"); break;
                case 0x28: stack.push("NaN"); break;

                case 0x24: { // pushbyte
                    if (pc < code.size()) {
                        i8 val = (i8)code[pc++];
                        stack.push(std::to_string((int)val));
                    }
                    break;
                }

                case 0x25: { // pushshort
                    u32 val = readU30FromBytes(code, pc);
                    stack.push(std::to_string(val));
                    break;
                }

                case 0x2C: { // pushstring
                    u32 idx = readU30FromBytes(code, pc);
                    stack.push("\"" + getString(idx) + "\"");
                    break;
                }

                case 0x2D: { // pushint
                    u32 idx = readU30FromBytes(code, pc);
                    if (idx < abc.cp.ints.size())
                        stack.push(std::to_string(abc.cp.ints[idx]));
                    else
                        stack.push("0");
                    break;
                }

                case 0x2E: { // pushuint
                    u32 idx = readU30FromBytes(code, pc);
                    if (idx < abc.cp.uints.size())
                        stack.push(std::to_string(abc.cp.uints[idx]));
                    else
                        stack.push("0");
                    break;
                }

                case 0x2F: { // pushdouble
                    u32 idx = readU30FromBytes(code, pc);
                    if (idx < abc.cp.doubles.size())
                        stack.push(std::to_string(abc.cp.doubles[idx]));
                    else
                        stack.push("0.0");
                    break;
                }

                case 0x30: // pushscope
                    if (!stack.empty()) stack.pop();
                    break;

                case 0xD0: stack.push("this"); break;
                case 0xD1: stack.push(locals.size() > 1 ? locals[1] : "arg1"); break;
                case 0xD2: stack.push(locals.size() > 2 ? locals[2] : "arg2"); break;
                case 0xD3: stack.push(locals.size() > 3 ? locals[3] : "arg3"); break;

                case 0x62: { // getlocal
                    u32 idx = readU30FromBytes(code, pc);
                    if (idx < locals.size())
                        stack.push("local" + std::to_string(idx));
                    else
                        stack.push("arg" + std::to_string(idx));
                    break;
                }

                case 0x63: { // setlocal
                    u32 idx = readU30FromBytes(code, pc);
                    if (!stack.empty()) {
                        out("var local" + std::to_string(idx) + " = " + stack.top() + ";");
                        if (idx < locals.size())
                            locals[idx] = "local" + std::to_string(idx);
                        stack.pop();
                    }
                    break;
                }

                case 0xD4: case 0xD5: case 0xD6: case 0xD7: {
                    u32 idx = op - 0xD4;
                    if (!stack.empty()) {
                        out("var local" + std::to_string(idx) + " = " + stack.top() + ";");
                        if (idx < locals.size())
                            locals[idx] = "local" + std::to_string(idx);
                        stack.pop();
                    }
                    break;
                }

                case 0xA0: { // add
                    if (stack.size() >= 2) {
                        std::string r = stack.top(); stack.pop();
                        std::string l = stack.top(); stack.pop();
                        stack.push("(" + l + " + " + r + ")");
                    }
                    break;
                }

                case 0xA1: { // subtract
                    if (stack.size() >= 2) {
                        std::string r = stack.top(); stack.pop();
                        std::string l = stack.top(); stack.pop();
                        stack.push("(" + l + " - " + r + ")");
                    }
                    break;
                }

                case 0xA2: { // multiply
                    if (stack.size() >= 2) {
                        std::string r = stack.top(); stack.pop();
                        std::string l = stack.top(); stack.pop();
                        stack.push("(" + l + " * " + r + ")");
                    }
                    break;
                }

                case 0xA3: { // divide
                    if (stack.size() >= 2) {
                        std::string r = stack.top(); stack.pop();
                        std::string l = stack.top(); stack.pop();
                        stack.push("(" + l + " / " + r + ")");
                    }
                    break;
                }

                case 0xAB: { // equals
                    if (stack.size() >= 2) {
                        std::string r = stack.top(); stack.pop();
                        std::string l = stack.top(); stack.pop();
                        stack.push("(" + l + " == " + r + ")");
                    }
                    break;
                }

                case 0xAD: { // lessthan
                    if (stack.size() >= 2) {
                        std::string r = stack.top(); stack.pop();
                        std::string l = stack.top(); stack.pop();
                        stack.push("(" + l + " < " + r + ")");
                    }
                    break;
                }

                case 0x60: { // getlex
                    u32 idx = readU30FromBytes(code, pc);
                    stack.push(getName(idx));
                    break;
                }

                case 0x66: { // getproperty
                    u32 idx = readU30FromBytes(code, pc);
                    if (!stack.empty()) {
                        std::string obj = stack.top(); stack.pop();
                        stack.push(obj + "." + getName(idx));
                    }
                    break;
                }

                case 0x61: // setproperty
                case 0x68: { // initproperty
                    u32 idx = readU30FromBytes(code, pc);
                    if (stack.size() >= 2) {
                        std::string val = stack.top(); stack.pop();
                        std::string obj = stack.top(); stack.pop();
                        out(obj + "." + getName(idx) + " = " + val + ";");
                    }
                    break;
                }

                case 0x46: { // callproperty
                    u32 idx = readU30FromBytes(code, pc);
                    u32 argc = readU30FromBytes(code, pc);
                    std::vector<std::string> args;
                    for (u32 i = 0; i < argc && !stack.empty(); i++) {
                        args.push_back(stack.top());
                        stack.pop();
                    }
                    std::reverse(args.begin(), args.end());
                    
                    if (!stack.empty()) {
                        std::string obj = stack.top(); stack.pop();
                        std::string call = obj + "." + getName(idx) + "(";
                        for (size_t i = 0; i < args.size(); i++) {
                            if (i > 0) call += ", ";
                            call += args[i];
                        }
                        call += ")";
                        stack.push(call);
                    }
                    break;
                }

                case 0x4F: { // callpropvoid
                    u32 idx = readU30FromBytes(code, pc);
                    u32 argc = readU30FromBytes(code, pc);
                    std::vector<std::string> args;
                    for (u32 i = 0; i < argc && !stack.empty(); i++) {
                        args.push_back(stack.top());
                        stack.pop();
                    }
                    std::reverse(args.begin(), args.end());
                    
                    if (!stack.empty()) {
                        std::string obj = stack.top(); stack.pop();
                        std::string call = obj + "." + getName(idx) + "(";
                        for (size_t i = 0; i < args.size(); i++) {
                            if (i > 0) call += ", ";
                            call += args[i];
                        }
                        call += ");";
                        out(call);
                    }
                    break;
                }

                case 0x40: { // newfunction
                    u32 idx = readU30FromBytes(code, pc);
                    stack.push("function_" + std::to_string(idx));
                    break;
                }

                case 0x55: { // newclass
                    u32 idx = readU30FromBytes(code, pc);
                    if (!stack.empty()) stack.pop();
                    stack.push("Class_" + std::to_string(idx));
                    break;
                }

                case 0x56: { // newobject
                    u32 argc = readU30FromBytes(code, pc);
                    for (u32 i = 0; i < argc * 2; i++) {
                        if (!stack.empty()) stack.pop();
                    }
                    stack.push("{}");
                    break;
                }

                case 0x57: { // newarray
                    u32 argc = readU30FromBytes(code, pc);
                    std::vector<std::string> items;
                    for (u32 i = 0; i < argc && !stack.empty(); i++) {
                        items.push_back(stack.top());
                        stack.pop();
                    }
                    std::reverse(items.begin(), items.end());
                    std::string arr = "[";
                    for (size_t i = 0; i < items.size(); i++) {
                        if (i > 0) arr += ", ";
                        arr += items[i];
                    }
                    arr += "]";
                    stack.push(arr);
                    break;
                }

                case 0x10: { // jump
                    i32 offset = readS24(code, pc);
                    size_t target = pc + offset;
                    jumpTargets.insert(target);
                    out("goto label_" + std::to_string(target) + ";");
                    break;
                }

                case 0x11: { // iftrue
                    i32 offset = readS24(code, pc);
                    size_t target = pc + offset;
                    jumpTargets.insert(target);
                    if (!stack.empty()) {
                        out("if (" + stack.top() + ") goto label_" + std::to_string(target) + ";");
                        stack.pop();
                    }
                    break;
                }

                case 0x12: { // iffalse
                    i32 offset = readS24(code, pc);
                    size_t target = pc + offset;
                    jumpTargets.insert(target);
                    if (!stack.empty()) {
                        out("if (!(" + stack.top() + ")) goto label_" + std::to_string(target) + ";");
                        stack.pop();
                    }
                    break;
                }


                case 0x29: // pop
                    if (!stack.empty()) {
                        out(stack.top() + ";");
                        stack.pop();
                    }
                    break;

                case 0x2A: // dup
                    if (!stack.empty()) {
                        stack.push(stack.top());
                    }
                    break;

                case 0x73: { // convert_i
                    if (!stack.empty()) {
                        std::string val = stack.top(); stack.pop();
                        stack.push("int(" + val + ")");
                    }
                    break;
                }

                case 0x74: { // convert_u
                    if (!stack.empty()) {
                        std::string val = stack.top(); stack.pop();
                        stack.push("uint(" + val + ")");
                    }
                    break;
                }

                case 0x75: { // convert_d
                    if (!stack.empty()) {
                        std::string val = stack.top(); stack.pop();
                        stack.push("Number(" + val + ")");
                    }
                    break;
                }

                //default:
                    // Unknown opcode - comment it
                    //out("// opcode 0x" + std::to_string(op));
                    //break;
                  default:
                      if (keepOpcodeComments) out("// opcode 0x" + std::to_string(op)); // unknown opcode comment only if flag is true
                      break;
            }
        }

        return output.str();
    }
};

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: abcdec_s2 file.abc\n";
        return 1;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "cannot open file\n";
        return 1;
    }

    // --- 1. HANDLE DoABC HEADER ---
    u32 flags;
    in.read((char*)&flags, 4);

    if (flags == 1) {
        std::cout << "Detected DoABC tag header. Skipping...\n";
        std::string name;
        std::getline(in, name, '\0'); 
    } else {
        in.seekg(0, std::ios::beg); // Rewind only if NO header was found
    }

    // --- 2. DIAGNOSTIC (STAY IN PLACE) ---
    std::cout << "--- START OF ABC DATA DIAGNOSIS ---\n";
    std::streampos startOfABC = in.tellg(); // Remember this spot
    u8 buffer[16];
    in.read((char*)buffer, 16);
    std::cout << "First 16 bytes: ";
    for(int i=0; i<16; i++) printf("%02X ", buffer[i]);
    std::cout << "\n";
    
    in.seekg(startOfABC); // Rewind ONLY to the start of the ABC data
    std::cout << "-----------------------------------\n";

    std::cout << "Parsing ABC..." << std::endl;
    ABCParser parser(in);
    ABC abc = parser.parse();

    std::unordered_map<u32, const MethodBody*> bodyMap;
    for (const auto& b : abc.bodies)
        bodyMap[b.method] = &b;

fs::create_directory("outputABC_decompiled");
    Decompiler dec(abc);

    //std::ofstream out("outputABC_decompiled/all_methods.as");
    //out << "// Total Methods Found: " << abc.bodies.size() << "\n\n";

for (const Script& s : abc.scripts) {
    for (const Trait& t : s.traits) {

        // Only class traits define classes
        if ((t.kind & 0x0F) != 4)
            continue;

        const ClassDef& cls = abc.classes[t.classIndex];

        std::string className = dec.getName(cls.instance.name);
        std::string package = dec.getPackage(cls.instance.name);

        // Create directory structure
        fs::path dir = "outputABC_decompiled";
        if (!package.empty()) {
            std::stringstream ss(package);
            std::string part;
            while (std::getline(ss, part, '.'))
                dir /= part;
        }
        fs::create_directories(dir);

        fs::path filePath = dir / (className + ".as");
        std::ofstream out(filePath);

        // Emit package + class
        if (!package.empty())
            out << "package " << package << " {\n";

        out << "public class " << className;

        if (cls.instance.superName != 0)
            out << " extends " << dec.getName(cls.instance.superName);

        out << " {\n";

        // ---- instance methods ----
        for (const Trait& mt : cls.instance.traits) {
            if ((mt.kind & 0x0F) >= 1 && (mt.kind & 0x0F) <= 3) {
                const MethodBody* body = nullptr;
                auto it = bodyMap.find(mt.methodIndex);
                if (it != bodyMap.end())
                    body = it->second;

                std::string mname = dec.getName(mt.name);
                out << "    public function " << mname << "() {\n";
                if (body)
                    out << dec.decompileMethod(*body);
                out << "    }\n\n";
            }
        }

        // ---- static methods ----
        for (const Trait& mt : cls.statics.traits) {
            if ((mt.kind & 0x0F) >= 1 && (mt.kind & 0x0F) <= 3) {
                const MethodBody* body = nullptr;
                auto it = bodyMap.find(mt.methodIndex);
                if (it != bodyMap.end())
                    body = it->second;

                std::string mname = dec.getName(mt.name);
                out << "    public static function " << mname << "() {\n";
                if (body)
                    out << dec.decompileMethod(*body);
                out << "    }\n\n";
            }
        }

        out << "}\n";
        if (!package.empty())
            out << "}\n";
    }
}


    //out.close();
    std::cout << "✓ Exported classes to outputABC_decompiled/\n";
    return 0;
}