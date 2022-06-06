#pragma

#include <string>
#include <functional>
#include <utility>

#include "../types.h"
#include "./instructions.h"
#include "./utils.h"

namespace ts::checker {

    using std::string;
    using std::function;
    using instructions::OP;

    enum class SymbolType {
        Variable,
        Function,
        Class,
        Type,
        TypeVariable //template variable
    };

    //A subroutine is a sub program that can be executed by knowing its address.
    //They are used for example for type alias, mapped type, conditional type (for false and true side)
    struct Subroutine {
        vector<unsigned char> ops; //OPs, and its parameters
        unordered_map<unsigned int, unsigned int> opSourceMap;
        string_view identifier;
        unsigned int address{}; //during compilation the index address, after the final address in the binary
        unsigned int pos{};
        SymbolType type = SymbolType::Type;

        explicit Subroutine(string_view &identifier): identifier(identifier) {}
    };

    struct Frame;

    struct Symbol {
        const string_view name;
        const SymbolType type;
        const unsigned int index{}; //symbol index of the current frame
        const unsigned int pos;
        unsigned int declarations = 1;
        Subroutine *routine = nullptr;
        shared<Frame> frame = nullptr;
    };

    struct Frame {
        const bool conditional = false;
        shared<Frame> previous;
        unsigned int id = 0; //in a tree the unique id, needed to resolve symbols during runtime.
        vector<Symbol> symbols{};

        Frame() = default;

        Frame(shared<Frame> previous): previous(std::move(previous)) {}
    };

    struct StorageItem {
        string_view value;
        unsigned int address{};

        explicit StorageItem(const string_view &value): value(value) {}
    };

    struct FrameOffset {
        uint32_t frame; //how many frames up
        uint32_t symbol; //the index of the symbol in referenced frame, refers directly to x stack entry of that stack frame.
    };

    class Program {
    public:
        vector<unsigned char> ops; //OPs of "main"
        unordered_map<unsigned int, unsigned int> opSourceMap;

        vector<string_view> storage; //all kind of literals, as strings
        unordered_map<string_view, reference_wrapper<StorageItem>> storageMap; //used to deduplicated storage entries
        unsigned int storageIndex{};
        shared<Frame> frame = make_shared<Frame>();

        //tracks which subroutine is active (end() is), so that pushOp calls are correctly assigned.
        vector<Subroutine *> activeSubroutines;
        vector<Subroutine> subroutines;

        //implicit is when a OP itself triggers in the VM a new frame, without having explicitly a OP::Frame
        shared<Frame> pushFrame(bool implicit = false) {
            if (!implicit) this->pushOp(OP::Frame);
            auto id = frame->id;
            frame = make_shared<Frame>(frame);
            frame->id = id + 1;
            return frame;
        }

        /**
         * Push the subroutine from the symbol as active. This means it will now be populated with OPs.
         */
        unsigned int pushSubroutine(string_view name) {
            //find subroutine
            for (auto &&s: frame->symbols) {
                if (s.name == name) {
                    pushFrame(true); //subroutines have implicit stack frames due to call convention
                    activeSubroutines.push_back(s.routine);
                    return s.routine->address;
                }
            }
            throw runtime_error(fmt::format("no symbol found for {}", name));
        }

        /**
         * Returns the index of the sub routine. Will be replaced in build() with the real address.
         */
        void popSubroutine() {
            if (activeSubroutines.empty()) throw runtime_error("No active subroutine found");
            popFrameImplicit();
            auto subroutine = activeSubroutines.back();
            if (subroutine->ops.empty()) {
                throw runtime_error("Routine is empty");
            }
            subroutine->ops.push_back(OP::Return);
            activeSubroutines.pop_back();
        }

        /**
         * The returning index will be replaced for all Call OP with the real subroutine address.
         */
        Symbol &findSymbol(const string_view &identifier) {
            Frame *current = frame.get();

            while (true) {
                for (auto &&s: current->symbols) {
                    if (s.name == identifier) {
                        return s;
                    }
                }
                if (!current->previous) break;
                current = current->previous.get();
            };

            throw runtime_error(fmt::format("No symbol for {} found", identifier));
        }

        /**
         * Remove stack without doing it as OP in the VM. Some other command calls popFrame() already, which makes popFrameImplicit() an implicit popFrame.
         * e.g. union, class, etc. all call VM::popFrame(). the current CompilerProgram needs to be aware of that, which this function is for.
         */
        void popFrameImplicit() {
            if (frame->previous) frame = frame->previous;
        }

        /**
         * The address is always written using 4 bytes.
         *
         * It sometimes is defined in Program as index to the storage or subroutine and thus is a immediate representation of the address.
         * In this case it will be replaced in build() with the real address in the binary (hence why we need 4 bytes, so space stays constant).
         */
        void pushAddress(unsigned int address) {
            auto &ops = getOPs();
            writeUint32(ops, ops.size(), address);
        }

        void pushSymbolAddress(Symbol &symbol) {
            auto &ops = getOPs();
            writeUint32(ops, ops.size(), symbol.frame->id);
            writeUint32(ops, ops.size(), symbol.index);
        }

        vector<unsigned char> &getOPs() {
            if (activeSubroutines.size()) return activeSubroutines.back()->ops;
            return ops;
        }

        void pushOp(OP op, std::vector<unsigned int> params = {}) {
            auto &ops = getOPs();
            ops.push_back(op);
            ops.insert(ops.end(), params.begin(), params.end());
        }

        //needed for variables
//        void pushOpAtFrameInHead(shared<Frame> frame, OP op, std::vector<unsigned int> params = {}) {
//            auto &ops = getOPs();
//
//            //an earlier known frame could be referenced, in which case we have to put ops between others.
//            ops.insert(ops.begin() + frame->headOffset, op);
//            if (params.size()) {
//                ops.insert(ops.begin() + frame->headOffset + 1, params.begin(), params.end());
//            }
//        }

        /**
         * A symbol could be type alias, function expression, var type declaration.
         * Each represents a type expression and gets its own subroutine. The subroutine
         * is directly created and an index assign. Later when pushSubroutine() is called,
         * this subroutine is returned and with OPs populated.
         *
         * Symbols will be created first before a body is extracted. This makes sure all
         * symbols are known before their reference is used.
         */
        Symbol &pushSymbol(string_view name, SymbolType type, unsigned int pos, shared<Frame> frameToUse = nullptr) {
            if (!frameToUse) frameToUse = frame;

            for (auto &&v: frameToUse->symbols) {
                if (v.name == name) {
                    v.declarations++;
                    return v;
                }
            }

            frameToUse->symbols.push_back(Symbol{
                    .frame = frameToUse,
                    .name = name,
                    .pos = pos,
                    .index = (unsigned int) frameToUse->symbols.size()
            });
            return frameToUse->symbols.back();
        }

        Symbol &pushSymbolForRoutine(string_view name, SymbolType type, unsigned int pos, shared<Frame> frameToUse = nullptr) {
            auto &symbol = pushSymbol(name, type, pos, frameToUse);
            if (symbol.routine) return symbol;

            Subroutine routine{name};
            routine.pos = pos;
            routine.type = type;
            routine.address = subroutines.size();
            subroutines.push_back(std::move(routine));
            symbol.routine = &subroutines.back();

            return symbol;
        }

        //note: make sure the same name is not added twice. needs hashmap
        unsigned int registerStorage(const string_view &s) {
            if (!storageIndex) storageIndex = 5; //jump+address

            const auto address = storageIndex;
            storage.push_back(s);
            storageIndex += 2 + s.size();
            return address;
        }

        void pushStorage(const string_view &s) {
            pushAddress(registerStorage(s));
        }

        string_view findStorage(unsigned int index) {
            unsigned int i = 5;
            for (auto &&s: storage) {
                if (i == index) return s;
                i += 2 + s.size();
            }
            return "!unknown";
        }

        string build() {
            vector<unsigned char> bin;
            unsigned int address = 0;

            if (storage.size() || subroutines.size()) {
                address = 5; //we add JUMP + index when building the program to jump over all subroutines&storages
                bin.push_back(OP::Jump);
                writeUint32(bin, bin.size(), 0); //set after routine handling
            }

            for (auto &&item: storage) {
                writeUint16(bin, address, item.size());
                bin.insert(bin.end(), item.begin(), item.end());
                address += 2 + item.size();
            }

            //detect final binary address of all subroutines
            unsigned int routineAddress = address;
            for (auto &&routine: subroutines) {
                routine.address = routineAddress;
                routineAddress += routine.ops.size();
            }

            //go through all OPs and adjust CALL parameter to the final binary address
            setFinalBinaryAddress(ops);
            for (auto &&routine: subroutines) {
                setFinalBinaryAddress(routine.ops);
            }

            for (auto &&routine: subroutines) {
                bin.insert(bin.end(), routine.ops.begin(), routine.ops.end());
                address += routine.ops.size();
            }

            writeUint32(bin, 1, address);

            bin.insert(bin.end(), ops.begin(), ops.end());

            return string(bin.begin(), bin.end());
        }

        void setFinalBinaryAddress(vector<unsigned char> &ops) {
            const auto end = ops.size();
            for (unsigned int i = 0; i < end; i++) {
                auto op = (OP) ops[i];
                switch (op) {
                    case OP::Call: {
                        //adjust binary address
                        auto index = readUint32(ops, i + 1);
                        auto &routine = subroutines[index];
                        writeUint32(ops, i + 1, routine.address);
                        i += 4;
                        break;
                    }
                    case OP::Loads: //2 bytes each: frame id + symbol index
                    case OP::NumberLiteral:
                    case OP::BigIntLiteral:
                    case OP::StringLiteral: {
                        i += 4;
                        break;
                    }
                }
            }
        }

        void printOps(vector<unsigned char> ops) {
            const auto end = ops.size();
            for (unsigned int i = 0; i < end; i++) {
                auto op = (OP) ops[i];
                std::string params = "";
                switch (op) {
                    case OP::Call: {
                        params += fmt::format(" &{}", readUint32(ops, i + 1));
                        i += 4;
                        break;
                    }
                    case OP::Loads:
                        params += fmt::format(" &{}:{}", readUint16(ops, i + 1), readUint16(ops, i + 3));
                        i += 4;
                        break;
                    case OP::NumberLiteral:
                    case OP::BigIntLiteral:
                    case OP::StringLiteral: {
                        params += fmt::format(" \"{}\"", findStorage(readUint32(ops, i + 1)));
                        i += 4;
                        break;
                    }
                }

                if (params.empty()) {
                    fmt::print("{} ", op);
                } else {
                    fmt::print("({}{}) ", op, params);
                }
            }
            fmt::print("\n");
        }

        void print() {
            int i = 0;
            for (auto &&subroutine: subroutines) {
                fmt::print("Subroutine {} &{}, {} bytes: ", subroutine.identifier, i++, subroutine.ops.size());
                printOps(subroutine.ops);
            }

            debug("Main {} bytes: {}", ops.size(), ops);
            printOps(ops);
        }
    };

    class Compiler {
    public:
        Program compileSourceFile(const shared<SourceFile> &file) {
            Program program;

            handle(file, program);

            return std::move(program);
        }

        void handle(const shared<Node> &node, Program &program) {
            switch (node->kind) {
                case types::SyntaxKind::SourceFile: {
                    for (auto &&statement: node->to<SourceFile>().statements->list) {
                        handle(statement, program);
                    }
                    break;
                }
                case types::SyntaxKind::BooleanKeyword: program.pushOp(OP::Boolean);
                    break;
                case types::SyntaxKind::StringKeyword: program.pushOp(OP::String);
                    break;
                case types::SyntaxKind::NumberKeyword: program.pushOp(OP::Number);
                    break;
                case types::SyntaxKind::BigIntLiteral: program.pushOp(OP::BigIntLiteral);
                    program.pushStorage(to<BigIntLiteral>(node)->text);
                    break;
                case types::SyntaxKind::NumericLiteral: program.pushOp(OP::NumberLiteral);
                    program.pushStorage(to<NumericLiteral>(node)->text);
                    break;
                case types::SyntaxKind::StringLiteral: program.pushOp(OP::StringLiteral);
                    program.pushStorage(to<StringLiteral>(node)->text);
                    break;
                case types::SyntaxKind::TrueKeyword: program.pushOp(OP::True);
                    break;
                case types::SyntaxKind::FalseKeyword: program.pushOp(OP::False);
                    break;
                case types::SyntaxKind::UnionType: {
                    const auto n = to<UnionTypeNode>(node);
                    program.pushFrame();

                    for (auto &&s: n->types->list) {
                        handle(s, program);
                    }

                    program.pushOp(OP::Union);
                    program.popFrameImplicit();
                    break;
                }
                case types::SyntaxKind::TypeReference: {
                    //todo: search in symbol table and get address
//                    debug("type reference {}", to<TypeReferenceNode>(node)->typeName->to<Identifier>().escapedText);
//                    program.pushOp(OP::Number);

                    const auto name = to<TypeReferenceNode>(node)->typeName->to<Identifier>().escapedText;
                    auto &symbol = program.findSymbol(name);
                    if (symbol.type == SymbolType::TypeVariable) {
                        program.pushOp(OP::Loads);
                        program.pushSymbolAddress(symbol);
                    } else {
                        program.pushOp(OP::Call);
                        program.pushAddress(symbol.routine->address);
                    }
                    break;
                }
                case types::SyntaxKind::TypeAliasDeclaration: {
                    const auto n = to<TypeAliasDeclaration>(node);

                    auto &symbol = program.pushSymbolForRoutine(n->name->escapedText, SymbolType::Type, n->pos); //move this to earlier symbol-scan round
                    if (symbol.declarations > 1) {
                        //todo: for functions/variable embed an error that symbol was declared twice in the same scope
                    } else {
                        //populate routine
                        program.pushSubroutine(n->name->escapedText);

                        //todo: extract type parameters
                        if (n->typeParameters) {
                            for (auto &&p: n->typeParameters->list) {
                                auto symbol = program.pushSymbol(n->name->escapedText, SymbolType::TypeVariable, n->pos);
                                program.pushOp(instructions::Var);
                            }
                        }

                        handle(n->type, program);
                        program.popSubroutine();
                    }
                    break;
                }
                case types::SyntaxKind::FunctionDeclaration: {
                    const auto n = to<FunctionDeclaration>(node);
                    if (const auto id = to<Identifier>(n->name)) {
                        auto &symbol = program.pushSymbolForRoutine(id->escapedText, SymbolType::Function, id->pos); //move this to earlier symbol-scan round
                        if (symbol.declarations > 1) {
                            //todo: embed error since function is declared twice
                        } else {
                            program.pushSubroutine(id->escapedText);

                            for (auto &&param: n->parameters->list) {
                                handle(n, program);
                            }
                            if (n->type) {
                                handle(n->type, program);
                            } else {
                                //todo: Infer from body
                                program.pushOp(OP::Unknown);
                                if (n->body) {
                                } else {
                                }
                            }

                            program.pushOp(OP::Function);
                            program.popSubroutine();
                        }
                    } else {
                        debug("No identifier in name");
                    }

                    break;
                }
                case types::SyntaxKind::VariableStatement: {
                    for (auto &&s: to<VariableStatement>(node)->declarationList->declarations->list) {
                        handle(s, program);
                    }
                    break;
                }
                case types::SyntaxKind::VariableDeclaration: {
                    const auto n = to<VariableDeclaration>(node);
                    if (const auto id = to<Identifier>(n->name)) {
                        auto &symbol = program.pushSymbolForRoutine(id->escapedText, SymbolType::Variable, id->pos); //move this to earlier symbol-scan round
                        if (symbol.declarations > 1) {
                            //todo: embed error since variable is declared twice
                        } else {
                            const auto subroutineIndex = program.pushSubroutine(id->escapedText);

                            if (n->type) {
                                handle(n->type, program);
                            } else {
                                program.pushOp(OP::Unknown);
                            }
                            program.popSubroutine();

                            if (n->initializer) {
                                //varName = initializer
                                handle(n->initializer, program);
                                program.pushOp(OP::Call);
                                program.pushAddress(subroutineIndex);
                                program.pushOp(OP::Assign);
                            }
                        }
                    } else {
                        debug("No identifier in name");
                    }
                    break;
                }
                default: {
                    debug("Node {} not handled", node->kind);
                }
            }
        }
    };
}