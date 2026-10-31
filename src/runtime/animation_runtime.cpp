#include "runtime/animation_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <esp_heap_caps.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "esp_system.h"

namespace ledlab {
namespace {

enum class TokenKind : uint8_t { Number, String, Identifier, Operator, End };

enum class BuiltinId : uint8_t { Unknown, Random, RandomRange, Array, Rgb, Hsv, Mix, Gradient, Noise, Sin, Cos, Tan, Abs, Sqrt, Floor, Ceil, Round, Exp, Log, Pow, Atan2, Min, Max, Fract, Clamp, Lerp, Smoothstep };
enum class BinaryOp : uint8_t { Unknown, Add, Subtract, Multiply, Divide, Modulo, Power, Less, Greater, LessEqual, GreaterEqual, Equal, NotEqual, And, Or };
BuiltinId builtinIdFor(const std::string& name) {
    if (name == "random") return BuiltinId::Random;
    if (name == "randomRange") return BuiltinId::RandomRange;
    if (name == "array") return BuiltinId::Array;
    if (name == "rgb") return BuiltinId::Rgb;
    if (name == "hsv") return BuiltinId::Hsv;
    if (name == "mix") return BuiltinId::Mix;
    if (name == "gradient") return BuiltinId::Gradient;
    if (name == "noise") return BuiltinId::Noise;
    if (name == "sin") return BuiltinId::Sin;
    if (name == "cos") return BuiltinId::Cos;
    if (name == "tan") return BuiltinId::Tan;
    if (name == "abs") return BuiltinId::Abs;
    if (name == "sqrt") return BuiltinId::Sqrt;
    if (name == "floor") return BuiltinId::Floor;
    if (name == "ceil") return BuiltinId::Ceil;
    if (name == "round") return BuiltinId::Round;
    if (name == "exp") return BuiltinId::Exp;
    if (name == "log") return BuiltinId::Log;
    if (name == "pow") return BuiltinId::Pow;
    if (name == "atan2") return BuiltinId::Atan2;
    if (name == "min") return BuiltinId::Min;
    if (name == "max") return BuiltinId::Max;
    if (name == "fract") return BuiltinId::Fract;
    if (name == "clamp") return BuiltinId::Clamp;
    if (name == "lerp") return BuiltinId::Lerp;
    if (name == "smoothstep") return BuiltinId::Smoothstep;
    return BuiltinId::Unknown;
}
BinaryOp binaryOpFor(const std::string& op) {
    if (op == "+") return BinaryOp::Add;
    if (op == "-") return BinaryOp::Subtract;
    if (op == "*") return BinaryOp::Multiply;
    if (op == "/") return BinaryOp::Divide;
    if (op == "%") return BinaryOp::Modulo;
    if (op == "^") return BinaryOp::Power;
    if (op == "<") return BinaryOp::Less;
    if (op == ">") return BinaryOp::Greater;
    if (op == "<=") return BinaryOp::LessEqual;
    if (op == ">=") return BinaryOp::GreaterEqual;
    if (op == "==") return BinaryOp::Equal;
    if (op == "!=") return BinaryOp::NotEqual;
    if (op == "&&") return BinaryOp::And;
    if (op == "||") return BinaryOp::Or;
    return BinaryOp::Unknown;
}

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    float number = 0;
    uint16_t line = 1;
    uint16_t column = 1;
};

struct SourceError : std::runtime_error {
    SourceError(const std::string& code, const std::string& message,
                uint16_t line = 0, uint16_t column = 0)
        : std::runtime_error(message), code(code), line(line), column(column) {}
    std::string code;
    uint16_t line;
    uint16_t column;
};

struct Value;
using Array = std::vector<Value>;

struct Value {
    enum class Kind : uint8_t { Number, Boolean, String, Array, Color };
    Kind kind = Kind::Number;
    float number = 0;
    bool boolean = false;
    std::string string;
    std::shared_ptr<Array> array;
    std::shared_ptr<std::vector<float>> numericArray;
    float color[3] = {0, 0, 0};

    static Value fromNumber(float value) {
        Value result;
        result.number = value;
        return result;
    }
    static Value fromBoolean(bool value) {
        Value result;
        result.kind = Kind::Boolean;
        result.boolean = value;
        return result;
    }
    static Value fromString(std::string value) {
        Value result;
        result.kind = Kind::String;
        result.string = std::move(value);
        return result;
    }
    static Value fromArray(Array value) {
        Value result;
        result.kind = Kind::Array;
        result.array = std::make_shared<Array>(std::move(value));
        return result;
    }
    static Value fromNumericArray(size_t length, float initial) {
        Value result;
        result.kind = Kind::Array;
        result.numericArray = std::make_shared<std::vector<float>>(length, initial);
        return result;
    }
    static Value fromColor(float red, float green, float blue) {
        Value result;
        result.kind = Kind::Color;
        result.color[0] = red;
        result.color[1] = green;
        result.color[2] = blue;
        return result;
    }
};

struct SourceLocation {
    uint16_t line = 1;
    uint16_t column = 1;
};

struct LocalEntry {
    const std::string* name = nullptr;
    Value value;
};

struct Workspace {
    std::array<LocalEntry, config::MAX_RUNTIME_LOCALS> locals{};
    std::array<Value, config::MAX_BUILTIN_ARGUMENTS> arguments{};
    size_t localCount = 0;
    size_t argumentTop = 0;

    // Locals never escape a single update() or render(i) invocation.
    void beginScope() {
        localCount = 0;
        argumentTop = 0;
    }
    Value* findLocal(const std::string& name) {
        for (size_t i = 0; i < localCount; ++i) {
            LocalEntry& entry = locals[i];
            if (entry.name && *entry.name == name) return &entry.value;
        }
        return nullptr;
    }
    const Value* findLocal(const std::string& name) const {
        for (size_t i = 0; i < localCount; ++i) {
            const LocalEntry& entry = locals[i];
            if (entry.name && *entry.name == name) return &entry.value;
        }
        return nullptr;
    }
    void setLocal(const std::string& name, Value value, SourceLocation location) {
        for (size_t i = 0; i < localCount; ++i) {
            LocalEntry& entry = locals[i];
            if (entry.name && *entry.name == name) {
                entry.value = std::move(value);
                return;
            }
        }
        if (localCount >= locals.size()) {
            throw SourceError("LOCAL_LIMIT", "Runtime local variable limit exceeded",
                              location.line, location.column);
        }
        LocalEntry& entry = locals[localCount++];
        entry.name = &name;
        entry.value = std::move(value);
    }
};

float numberOf(const Value& value) {
    if (value.kind == Value::Kind::Boolean) return value.boolean ? 1 : 0;
    if (value.kind == Value::Kind::Number) return value.number;
    if (value.kind == Value::Kind::String) return strtof(value.string.c_str(), nullptr);
    return 0;
}

bool isArrayValue(const Value& value) {
    return value.kind == Value::Kind::Array && (value.array || value.numericArray);
}

size_t arrayLength(const Value& value) {
    if (value.numericArray) return value.numericArray->size();
    return value.array ? value.array->size() : 0;
}

Value arrayElement(const Value& value, size_t index) {
    if (value.numericArray) return Value::fromNumber((*value.numericArray)[index]);
    return (*value.array)[index];
}

void setArrayElement(const Value& value, size_t index, const Value& element) {
    if (value.numericArray) {
        (*value.numericArray)[index] = numberOf(element);
    } else {
        (*value.array)[index] = element;
    }
}

bool truthy(const Value& value) {
    switch (value.kind) {
        case Value::Kind::Boolean: return value.boolean;
        case Value::Kind::Number: return value.number != 0 && !std::isnan(value.number);
        case Value::Kind::String: return !value.string.empty();
        case Value::Kind::Array:
        case Value::Kind::Color: return true;
    }
    return false;
}

struct Node {
    enum class Kind : uint8_t {
        Program, State, Update, Render, Block, Let, Return, Expression,
        Assign, If, For, Literal, Variable, Array, Unary, Binary,
        Conditional, Call, Index
    };
    enum class LiteralKind : uint8_t { Number, String, Boolean };
    explicit Node(Kind kind, const Token& token = Token{})
        : kind(kind), location{token.line, token.column} {}
    Kind kind;
    LiteralKind literalKind = LiteralKind::Number;
    SourceLocation location;
    float number = 0.0f;
    bool boolean = false;
    char operation = 0;
    BuiltinId builtin = BuiltinId::Unknown;
    BinaryOp binaryOp = BinaryOp::Unknown;
    std::string text;
    std::string auxiliary;
    std::vector<std::unique_ptr<Node>> children;
};

using NodePtr = std::unique_ptr<Node>;

class Lexer {
public:
    explicit Lexer(const std::string& source) : source_(source) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        tokens.reserve(std::min<size_t>(source_.size() / 4, config::MAX_TOKENS));
        while (position_ < source_.size()) {
            const char c = source_[position_];
            if (c == '\n') {
                advance();
                ++line_;
                column_ = 1;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f') {
                advance();
                continue;
            }
            if (c == '/' && peek(1) == '/') {
                while (position_ < source_.size() && source_[position_] != '\n') advance();
                continue;
            }

            const uint16_t startLine = line_;
            const uint16_t startColumn = column_;
            if (c == '"' || c == '\'') {
                tokens.push_back(readString(c, startLine, startColumn));
            } else if (isDigit(c) || (c == '.' && isDigit(peek(1)))) {
                tokens.push_back(readNumber(startLine, startColumn));
            } else if (isIdentifierStart(c)) {
                tokens.push_back(readIdentifier(startLine, startColumn));
            } else {
                tokens.push_back(readOperator(startLine, startColumn));
            }
            if (tokens.size() >= config::MAX_TOKENS) {
                throw SourceError("TOKEN_LIMIT", "Token limit exceeded", line_, column_);
            }
        }
        tokens.push_back(Token{TokenKind::End, "", 0, line_, column_});
        return tokens;
    }

private:
    static bool isDigit(char c) { return c >= '0' && c <= '9'; }
    static bool isIdentifierStart(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    static bool isIdentifierPart(char c) {
        return isIdentifierStart(c) || isDigit(c);
    }
    char peek(size_t offset) const {
        const size_t index = position_ + offset;
        return index < source_.size() ? source_[index] : '\0';
    }
    void advance() {
        ++position_;
        ++column_;
    }
    Token readString(char quote, uint16_t line, uint16_t column) {
        advance();
        std::string value;
        while (position_ < source_.size() && source_[position_] != quote) {
            char c = source_[position_];
            if (c == '\\' && position_ + 1 < source_.size()) {
                advance();
                c = source_[position_];
                value.push_back(c == 'n' ? '\n' : c == 't' ? '\t' : c);
                advance();
            } else {
                if (c == '\n') {
                    throw SourceError("LEX_ERROR", "Newline in string literal", line_, column_);
                }
                value.push_back(c);
                advance();
            }
        }
        if (position_ >= source_.size()) {
            throw SourceError("LEX_ERROR", "Unterminated string literal", line, column);
        }
        advance();
        return Token{TokenKind::String, std::move(value), 0, line, column};
    }
    Token readNumber(uint16_t line, uint16_t column) {
        const size_t start = position_;
        bool decimal = false;
        if (source_[position_] == '.') {
            decimal = true;
            advance();
        }
        while (isDigit(peek(0))) advance();
        if (!decimal && peek(0) == '.') {
            advance();
            while (isDigit(peek(0))) advance();
        }
        if (peek(0) == 'e' || peek(0) == 'E') {
            advance();
            if (peek(0) == '+' || peek(0) == '-') advance();
            if (!isDigit(peek(0))) {
                throw SourceError("LEX_ERROR", "Invalid number exponent", line_, column_);
            }
            while (isDigit(peek(0))) advance();
        }
        const float value = strtof(source_.c_str() + start, nullptr);
        return Token{TokenKind::Number, "", value, line, column};
    }
    Token readIdentifier(uint16_t line, uint16_t column) {
        const size_t start = position_;
        while (isIdentifierPart(peek(0))) advance();
        return Token{TokenKind::Identifier, source_.substr(start, position_ - start),
                     0, line, column};
    }
    Token readOperator(uint16_t line, uint16_t column) {
        static const char* const operators[] = {
            "+=", "-=", "*=", "/=", "<=", ">=", "==", "!=", "&&", "||",
            "+", "-", "*", "/", "%", "^", "<", ">", "!", "=", "?",
            ":", ";", ",", ".", "(", ")", "[", "]", "{", "}"
        };
        for (const char* op : operators) {
            const size_t length = strlen(op);
            if (source_.compare(position_, length, op) == 0) {
                for (size_t i = 0; i < length; ++i) advance();
                return Token{TokenKind::Operator, op, 0, line, column};
            }
        }
        const std::string bad(1, source_[position_]);
        throw SourceError("LEX_ERROR", "Unknown character: " + bad, line, column);
    }

    const std::string& source_;
    size_t position_ = 0;
    uint16_t line_ = 1;
    uint16_t column_ = 1;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    NodePtr parse() {
        expect("animation");
        const Token name = expectKind(TokenKind::String, "Animation name must be quoted");
        NodePtr program = make(Node::Kind::Program, name);
        program->text = name.text;
        expect("{");
        bool hasUpdate = false;
        bool hasRender = false;
        while (!check("}")) {
            if (isEnd()) fail(peek(), "Unclosed animation body");
            if (match("state")) {
                Token stateToken = previous();
                NodePtr state = make(Node::Kind::State, stateToken);
                state->text = expectKind(TokenKind::Identifier, "Expected state identifier").text;
                expect("=");
                state->children.push_back(expression());
                expect(";");
                program->children.push_back(std::move(state));
            } else if (match("update")) {
                if (hasUpdate) fail(previous(), "Duplicate update declaration");
                hasUpdate = true;
                program->children.push_back(parseFunction(Node::Kind::Update));
            } else if (match("render")) {
                if (hasRender) fail(previous(), "Duplicate render declaration");
                hasRender = true;
                program->children.push_back(parseFunction(Node::Kind::Render));
            } else {
                fail(peek(), "Unknown animation declaration: " + peek().text);
            }
        }
        expect("}");
        if (!isEnd()) fail(peek(), "Unexpected code after animation");
        if (!hasRender) fail(name, "Missing required render(i) function");
        return program;
    }

private:
    NodePtr make(Node::Kind kind, const Token& token = Token{}) {
        if (++nodeCount_ > config::MAX_AST_NODES) {
            fail(token, "AST node limit exceeded", "AST_LIMIT");
        }
        return std::make_unique<Node>(kind, token);
    }
    const Token& peek(size_t offset = 0) const {
        return tokens_[std::min(position_ + offset, tokens_.size() - 1)];
    }
    const Token& previous() const { return tokens_[position_ - 1]; }
    bool isEnd() const { return peek().kind == TokenKind::End; }
    bool check(const char* text) const { return peek().text == text; }
    bool match(const char* text) {
        if (!check(text)) return false;
        ++position_;
        return true;
    }
    Token expect(const char* text) {
        if (!check(text)) fail(peek(), std::string("Expected '") + text + "', got '" + peek().text + "'");
        return tokens_[position_++];
    }
    Token expectKind(TokenKind kind, const char* message) {
        if (peek().kind != kind) fail(peek(), message);
        return tokens_[position_++];
    }
    [[noreturn]] void fail(const Token& token, const std::string& message,
                           const char* code = "PARSE_ERROR") const {
        throw SourceError(code, message, token.line, token.column);
    }
    NodePtr parseFunction(Node::Kind kind) {
        const Token declaration = previous();
        NodePtr function = make(kind, declaration);
        expect("(");
        function->text = expectKind(TokenKind::Identifier, "Expected function parameter").text;
        expect(")");
        function->children.push_back(block());
        return function;
    }
    NodePtr block() {
        const Token opening = expect("{");
        NodePtr result = make(Node::Kind::Block, opening);
        while (!check("}")) {
            if (isEnd()) fail(peek(), "Unclosed block");
            result->children.push_back(statement());
        }
        expect("}");
        return result;
    }
    NodePtr statement() {
        if (match("let")) {
            const Token token = previous();
            NodePtr node = make(Node::Kind::Let, token);
            node->text = expectKind(TokenKind::Identifier, "Expected variable identifier").text;
            expect("=");
            node->children.push_back(expression());
            expect(";");
            return node;
        }
        if (match("return")) {
            NodePtr node = make(Node::Kind::Return, previous());
            node->children.push_back(expression());
            expect(";");
            return node;
        }
        if (match("if")) {
            NodePtr node = make(Node::Kind::If, previous());
            expect("(");
            node->children.push_back(expression());
            expect(")");
            node->children.push_back(block());
            if (match("else")) node->children.push_back(block());
            else node->children.push_back(make(Node::Kind::Block, previous()));
            return node;
        }
        if (match("for")) return forStatement(previous());

        NodePtr target = expression();
        if (check("=") || check("+=") || check("-=") || check("*=") || check("/=")) {
            Token operation = tokens_[position_++];
            NodePtr node = make(Node::Kind::Assign, operation);
            node->operation = operation.text[0];
            node->text = operation.text;
            node->children.push_back(std::move(target));
            node->children.push_back(expression());
            expect(";");
            return node;
        }
        NodePtr node = make(Node::Kind::Expression);
        node->location = target->location;
        node->children.push_back(std::move(target));
        expect(";");
        return node;
    }
    NodePtr forStatement(const Token& token) {
        NodePtr node = make(Node::Kind::For, token);
        expect("(");
        expect("let");
        node->text = expectKind(TokenKind::Identifier, "Expected loop variable").text;
        expect("=");
        node->children.push_back(expression());
        expect(";");
        node->children.push_back(expression());
        expect(";");
        node->auxiliary = expectKind(TokenKind::Identifier, "Expected loop step variable").text;
        if (!check("+=") && !check("-=")) fail(peek(), "For loops support only += and -=");
        node->operation = tokens_[position_++].text[0];
        node->children.push_back(expression());
        expect(")");
        node->children.push_back(block());
        return node;
    }
    int precedence(const Token& token) const {
        if (token.kind != TokenKind::Operator) return 0;
        const std::string& op = token.text;
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        if (op == "^") return 7;
        return 0;
    }
    NodePtr expression(int minimum = 0, uint16_t depth = 0) {
        if (depth >= config::MAX_EXPRESSION_DEPTH) {
            fail(peek(), "Expression nesting limit exceeded", "DEPTH_LIMIT");
        }
        NodePtr left = prefix(depth + 1);
        while (true) {
            if (check("?") && minimum < 1) {
                const Token operation = tokens_[position_++];
                NodePtr conditional = make(Node::Kind::Conditional, operation);
                conditional->children.push_back(std::move(left));
                conditional->children.push_back(expression(0, depth + 1));
                expect(":");
                conditional->children.push_back(expression(0, depth + 1));
                left = std::move(conditional);
                continue;
            }
            const int current = precedence(peek());
            if (current <= minimum) break;
            const Token operation = tokens_[position_++];
            NodePtr binary = make(Node::Kind::Binary, operation);
            binary->binaryOp = binaryOpFor(operation.text);
            binary->text = operation.text;
            binary->children.push_back(std::move(left));
            binary->children.push_back(expression(current - (operation.text == "^" ? 1 : 0), depth + 1));
            left = std::move(binary);
        }
        return left;
    }
    NodePtr prefix(uint16_t depth) {
        const Token token = tokens_[position_++];
        NodePtr node;
        if (token.kind == TokenKind::Number) {
            node = make(Node::Kind::Literal, token);
            node->literalKind = Node::LiteralKind::Number;
            node->number = token.number;
        } else if (token.kind == TokenKind::String) {
            node = make(Node::Kind::Literal, token);
            node->literalKind = Node::LiteralKind::String;
            node->text = token.text;
        } else if (token.text == "true" || token.text == "false") {
            node = make(Node::Kind::Literal, token);
            node->literalKind = Node::LiteralKind::Boolean;
            node->boolean = token.text == "true";
        } else if (token.text == "-" || token.text == "!") {
            node = make(Node::Kind::Unary, token);
            node->operation = token.text[0];
            node->text = token.text;
            node->children.push_back(expression(7, depth + 1));
        } else if (token.text == "(") {
            node = expression(0, depth + 1);
            expect(")");
        } else if (token.text == "[") {
            node = make(Node::Kind::Array, token);
            if (!check("]")) {
                do {
                    node->children.push_back(expression(0, depth + 1));
                } while (match(","));
            }
            expect("]");
        } else if (token.kind == TokenKind::Identifier) {
            node = make(Node::Kind::Variable, token);
            node->text = token.text;
        } else {
            fail(token, "Unexpected expression: " + token.text);
        }

        while (check("(") || check("[")) {
            if (match("(")) {
                if (node->kind != Node::Kind::Variable) {
                    fail(token, "Only built-in function calls are supported");
                }
                NodePtr call = make(Node::Kind::Call, token);
                call->text = node->text;
                call->builtin = builtinIdFor(call->text);
                if (!check(")")) {
                    do {
                        call->children.push_back(expression(0, depth + 1));
                    } while (match(","));
                }
                expect(")");
                node = std::move(call);
            } else {
                const Token opening = tokens_[position_++];
                NodePtr index = make(Node::Kind::Index, opening);
                index->children.push_back(std::move(node));
                index->children.push_back(expression(0, depth + 1));
                expect("]");
                node = std::move(index);
            }
        }
        return node;
    }

    std::vector<Token> tokens_;
    size_t position_ = 0;
    uint32_t nodeCount_ = 0;
};

struct Environment {
    const std::unordered_map<std::string, Value>& state;
    Workspace& workspace;
    float time = 0;
};

struct Budget {
    uint32_t remaining;
    uint32_t deadlineUs;
    uint32_t operations = 0;
    void consume(SourceLocation location) {
        if (remaining-- == 0) {
            throw SourceError("INSTRUCTION_LIMIT", "Instruction budget exceeded",
                              location.line, location.column);
        }
        if ((++operations & 127U) == 0U &&
            static_cast<int32_t>(micros() - deadlineUs) >= 0) {
            throw SourceError("TIME_LIMIT", "Frame execution time limit exceeded",
                              location.line, location.column);
        }
    }
};

Value resolveVariable(const std::string& name, Environment& environment,
                      SourceLocation location) {
    if (name == "time") return Value::fromNumber(environment.time);
    if (name == "PI") return Value::fromNumber(3.14159265358979323846f);
    const Value* local = environment.workspace.findLocal(name);
    if (local) return *local;
    auto state = environment.state.find(name);
    if (state != environment.state.end()) return state->second;
    throw SourceError("UNKNOWN_VARIABLE", "Unknown variable: " + name,
                      location.line, location.column);
}

Value numericOrStringPlus(const Value& a, const Value& b) {
    if (a.kind == Value::Kind::String || b.kind == Value::Kind::String) {
        const auto asText = [](const Value& value) {
            if (value.kind == Value::Kind::String) return value.string;
            if (value.kind == Value::Kind::Boolean) return value.boolean ? std::string("true") : std::string("false");
            return std::to_string(numberOf(value));
        };
        return Value::fromString(asText(a) + asText(b));
    }
    return Value::fromNumber(numberOf(a) + numberOf(b));
}

float clamped(float value, float minimum, float maximum) {
    return std::max(minimum, std::min(maximum, value));
}

float randomUnit() {
    return static_cast<float>(esp_random() >> 8U) * (1.0f / 16777216.0f);
}

Value callBuiltin(BuiltinId builtin, const std::string& name,
                  const Value* args, size_t count, SourceLocation location) {
    const auto arg = [args, count](size_t index, float fallback = 0.0f) {
        return index < count ? numberOf(args[index]) : fallback;
    };
    const auto require = [count, location, &name](size_t required) {
        if (count < required) {
            throw SourceError("ARGUMENT_ERROR", name + " requires at least " +
                              std::to_string(required) + " argument(s)",
                              location.line, location.column);
        }
    };
    switch (builtin) {
        case BuiltinId::Random: {
        return Value::fromNumber(randomUnit());
    }
        case BuiltinId::RandomRange: {
        require(2);
        return Value::fromNumber(arg(0) + (arg(1) - arg(0)) * randomUnit());
    }
        case BuiltinId::Array: {
        require(1);
        const float requestedLength = std::floor(arg(0));
        const int length = static_cast<int>(std::max(0.0f, std::min(
            static_cast<float>(config::MAX_ARRAY_LENGTH), requestedLength)));
        const float initial = arg(1);
        return Value::fromNumericArray(static_cast<size_t>(length), initial);
    }
        case BuiltinId::Rgb: {
        require(3);
        return Value::fromColor(clamped(arg(0), 0, 1),
                                clamped(arg(1), 0, 1),
                                clamped(arg(2), 0, 1));
    }
        case BuiltinId::Hsv: {
        require(3);
        const float h = std::fmod(std::fmod(arg(0), 1.0f) + 1.0f, 1.0f);
        const float s = arg(1);
        const float v = arg(2);
        const auto channel = [h, s, v](float n) {
            const float k = std::fmod(n + h * 6.0f, 6.0f);
            return v - v * s * std::max(0.0f, std::min(std::min(k, 4.0f - k), 1.0f));
        };
        return Value::fromColor(channel(5), channel(3), channel(1));
    }
        case BuiltinId::Mix: {
        require(3);
        if (args[0].kind == Value::Kind::Color && args[1].kind == Value::Kind::Color) {
            const float amount = arg(2);
            return Value::fromColor(
                args[0].color[0] + (args[1].color[0] - args[0].color[0]) * amount,
                args[0].color[1] + (args[1].color[1] - args[0].color[1]) * amount,
                args[0].color[2] + (args[1].color[2] - args[0].color[2]) * amount);
        }
        const auto colorOrArray = [](const Value& value) {
            return value.kind == Value::Kind::Color ||
                   (value.kind == Value::Kind::Array && (value.array || value.numericArray));
        };
        if (colorOrArray(args[0]) && colorOrArray(args[1])) {
            Array result;
            const auto lengthOf = [](const Value& value) {
                return value.kind == Value::Kind::Color ? size_t{3} : arrayLength(value);
            };
            const auto channelOf = [](const Value& value, size_t index) {
                return value.kind == Value::Kind::Color
                           ? value.color[index]
                           : numberOf(arrayElement(value, index));
            };
            const size_t length = std::min(lengthOf(args[0]), lengthOf(args[1]));
            result.reserve(length);
            for (size_t i = 0; i < length; ++i) {
                const float a = channelOf(args[0], i);
                result.push_back(Value::fromNumber(a + (channelOf(args[1], i) - a) * arg(2)));
            }
            if (args[0].kind == Value::Kind::Color && args[1].kind == Value::Kind::Color) {
                return Value::fromColor(numberOf(result[0]), numberOf(result[1]), numberOf(result[2]));
            }
            return Value::fromArray(std::move(result));
        }
        return Value::fromNumber(arg(0) + (arg(1) - arg(0)) * arg(2));
    }
        case BuiltinId::Gradient: {
        require(2);
        if (args[1].kind != Value::Kind::Array || !args[1].array || args[1].array->empty()) {
            return Value::fromArray({Value::fromNumber(0), Value::fromNumber(0), Value::fromNumber(0)});
        }
        const float position = arg(0);
        const auto stopValue = [](const Value& stop, size_t index) -> const Value* {
            if (stop.kind != Value::Kind::Array || !stop.array || stop.array->size() <= index) return nullptr;
            return &(*stop.array)[index];
        };
        size_t firstIndex = 0;
        size_t lastIndex = 0;
        float firstPosition = std::numeric_limits<float>::infinity();
        float lastPosition = -std::numeric_limits<float>::infinity();
        for (size_t i = 0; i < args[1].array->size(); ++i) {
            const Value* stop = &(*args[1].array)[i];
            const Value* stopPosition = stopValue(*stop, 0);
            if (!stopPosition || !stopValue(*stop, 1)) {
                throw SourceError("TYPE_ERROR", "Invalid gradient stop",
                                  location.line, location.column);
            }
            const float value = numberOf(*stopPosition);
            if (value < firstPosition) {
                firstPosition = value;
                firstIndex = i;
            }
            if (value > lastPosition) {
                lastPosition = value;
                lastIndex = i;
            }
        }
        const Value& first = (*args[1].array)[firstIndex];
        if (position <= firstPosition) return *stopValue(first, 1);
        const Value& last = (*args[1].array)[lastIndex];
        if (position >= lastPosition) return *stopValue(last, 1);
        const Value* leftPosition = &(*args[1].array)[firstIndex];
        const Value* leftColor = stopValue(*leftPosition, 1);
        const Value* rightPosition = nullptr;
        const Value* rightColor = nullptr;
        float lower = firstPosition;
        float upper = lastPosition;
        for (const Value& stop : *args[1].array) {
            const float value = numberOf(*stopValue(stop, 0));
            if (value > firstPosition && value <= position && value >= lower) {
                lower = value;
                leftColor = stopValue(stop, 1);
            }
            if (value > position && value < upper) {
                upper = value;
                rightPosition = &stop;
                rightColor = stopValue(stop, 1);
            }
        }
        if (!rightPosition || !rightColor || !leftColor) return *stopValue(last, 1);
        const float span = upper - lower;
        const float amount = span == 0 ? 0 : (position - lower) / span;
        const Value mixArgs[] = {*leftColor, *rightColor, Value::fromNumber(amount)};
        return callBuiltin(BuiltinId::Mix, "mix", mixArgs, 3, location);
    }
        case BuiltinId::Noise: {
        require(1);
        const float x = arg(0);
        const float y = arg(1);
        const float floorX = std::floor(x);
        const float floorY = std::floor(y);
        const auto toUint32 = [](float value) {
            if (!std::isfinite(value)) return uint32_t{0};
            float wrapped = std::fmod(std::trunc(value), 4294967296.0f);
            if (wrapped < 0.0f) wrapped += 4294967296.0f;
            return static_cast<uint32_t>(wrapped);
        };
        const uint32_t ix = toUint32(floorX);
        const uint32_t iy = toUint32(floorY);
        const float fx = x - floorX;
        const float fy = y - floorY;
        const auto hash = [](uint32_t a, uint32_t b) {
            uint32_t n = a * 374761393U + b * 668265263U;
            n = (n ^ (n >> 13U)) * 1274126177U;
            return static_cast<float>(n ^ (n >> 16U)) / 4294967295.0f;
        };
        const auto fade = [](float value) { return value * value * (3 - 2 * value); };
        const float a = hash(ix, iy);
        const float b = hash(ix + 1U, iy);
        const float c = hash(ix, iy + 1U);
        const float d = hash(ix + 1U, iy + 1U);
        const float u = fade(fx);
        const float v = fade(fy);
        return Value::fromNumber((a + (b - a) * u) * (1 - v) +
                                 (c + (d - c) * u) * v);
    }

        case BuiltinId::Sin: require(1); return Value::fromNumber(std::sin(arg(0)));
        case BuiltinId::Cos: require(1); return Value::fromNumber(std::cos(arg(0)));
        case BuiltinId::Tan: require(1); return Value::fromNumber(std::tan(arg(0)));
        case BuiltinId::Abs: require(1); return Value::fromNumber(std::fabs(arg(0)));
        case BuiltinId::Sqrt: require(1); return Value::fromNumber(std::sqrt(arg(0)));
        case BuiltinId::Floor: require(1); return Value::fromNumber(std::floor(arg(0)));
        case BuiltinId::Ceil: require(1); return Value::fromNumber(std::ceil(arg(0)));
        case BuiltinId::Round: require(1); return Value::fromNumber(std::round(arg(0)));
        case BuiltinId::Exp: require(1); return Value::fromNumber(std::exp(arg(0)));
        case BuiltinId::Log: require(1); return Value::fromNumber(std::log(arg(0)));
        case BuiltinId::Pow: {
        require(2);
        return Value::fromNumber(std::pow(arg(0), arg(1)));
    }
        case BuiltinId::Atan2: {
        require(2);
        return Value::fromNumber(std::atan2(arg(0), arg(1)));
    }
        case BuiltinId::Min:
        case BuiltinId::Max: {
        float result = arg(0);
        for (size_t i = 1; i < count; ++i) {
            result = builtin == BuiltinId::Min ? std::min(result, arg(i)) : std::max(result, arg(i));
        }
        return Value::fromNumber(result);
    }
        case BuiltinId::Fract: require(1); return Value::fromNumber(arg(0) - std::floor(arg(0)));
        case BuiltinId::Clamp: {
        require(3);
        return Value::fromNumber(std::max(arg(1), std::min(arg(2), arg(0))));
    }
        case BuiltinId::Lerp: {
        require(3);
        return Value::fromNumber(arg(0) + (arg(1) - arg(0)) * arg(2));
    }
        case BuiltinId::Smoothstep: {
        require(3);
        const float span = arg(1) - arg(0);
        const float t = span == 0 ? 0 : clamped((arg(2) - arg(0)) / span, 0, 1);
        return Value::fromNumber(t * t * (3 - 2 * t));
    }
        default:
            throw SourceError("UNKNOWN_FUNCTION", "Unknown function: " + name,
                              location.line, location.column);
    }
}

Value evaluate(const Node& node, Environment& environment, Budget& budget) {
    budget.consume(node.location);
    switch (node.kind) {
        case Node::Kind::Literal:
            if (node.literalKind == Node::LiteralKind::String) return Value::fromString(node.text);
            if (node.literalKind == Node::LiteralKind::Boolean) return Value::fromBoolean(node.boolean);
            return Value::fromNumber(node.number);
        case Node::Kind::Variable: return resolveVariable(node.text, environment, node.location);
        case Node::Kind::Array: {
            Array values;
            values.reserve(node.children.size());
            for (const NodePtr& child : node.children) values.push_back(evaluate(*child, environment, budget));
            return Value::fromArray(std::move(values));
        }
        case Node::Kind::Unary: {
            const Value value = evaluate(*node.children[0], environment, budget);
            return node.operation == '-' ? Value::fromNumber(-numberOf(value))
                                          : Value::fromBoolean(!truthy(value));
        }
        case Node::Kind::Conditional: {
            const Value condition = evaluate(*node.children[0], environment, budget);
            return evaluate(*node.children[truthy(condition) ? 1 : 2], environment, budget);
        }
        case Node::Kind::Binary: {
            const Value left = evaluate(*node.children[0], environment, budget);
            if (node.binaryOp == BinaryOp::And && !truthy(left)) return Value::fromBoolean(false);
            if (node.binaryOp == BinaryOp::Or && truthy(left)) return Value::fromBoolean(true);
            const Value right = evaluate(*node.children[1], environment, budget);
            const float a = numberOf(left);
            const float b = numberOf(right);
            switch (node.binaryOp) {
                case BinaryOp::Add: return numericOrStringPlus(left, right);
                case BinaryOp::Subtract: return Value::fromNumber(a - b);
                case BinaryOp::Multiply: return Value::fromNumber(a * b);
                case BinaryOp::Divide: return Value::fromNumber(b == 0 ? 0 : a / b);
                case BinaryOp::Modulo: return Value::fromNumber(b == 0 ? 0 : std::fmod(a, b));
                case BinaryOp::Power: return Value::fromNumber(std::pow(a, b));
                case BinaryOp::Less: return Value::fromBoolean(a < b);
                case BinaryOp::Greater: return Value::fromBoolean(a > b);
                case BinaryOp::LessEqual: return Value::fromBoolean(a <= b);
                case BinaryOp::GreaterEqual: return Value::fromBoolean(a >= b);
                case BinaryOp::Equal:
                case BinaryOp::NotEqual: {
                    bool equal = false;
                    if (left.kind == right.kind) {
                        if (left.kind == Value::Kind::String) equal = left.string == right.string;
                        else if (left.kind == Value::Kind::Boolean) equal = left.boolean == right.boolean;
                        else if (left.kind == Value::Kind::Number) equal = a == b;
                        else if (left.kind == Value::Kind::Color) {
                            equal = left.color[0] == right.color[0] &&
                                    left.color[1] == right.color[1] &&
                                    left.color[2] == right.color[2];
                        } else {
                            equal = left.array == right.array &&
                                    left.numericArray == right.numericArray;
                        }
                    }
                    return Value::fromBoolean(node.binaryOp == BinaryOp::Equal ? equal : !equal);
                }
                case BinaryOp::And: return Value::fromBoolean(truthy(left) && truthy(right));
                case BinaryOp::Or: return Value::fromBoolean(truthy(left) || truthy(right));
                default: break;
            }
            break;
        }
        case Node::Kind::Index: {
            const Value base = evaluate(*node.children[0], environment, budget);
            const float indexValue = std::floor(numberOf(evaluate(*node.children[1], environment, budget)));
            if (base.kind == Value::Kind::Color) {
                if (!std::isfinite(indexValue) || indexValue < 0 || indexValue >= 3) {
                    return Value::fromNumber(0);
                }
                return Value::fromNumber(base.color[static_cast<size_t>(indexValue)]);
            }
            if (!isArrayValue(base) || !std::isfinite(indexValue) ||
                indexValue < 0 || indexValue >= static_cast<float>(arrayLength(base))) {
                return Value::fromNumber(0);
            }
            return arrayElement(base, static_cast<size_t>(indexValue));
        }
        case Node::Kind::Call: {
            if (node.children.size() > environment.workspace.arguments.size()) {
                throw SourceError("ARGUMENT_LIMIT", "Too many built-in function arguments",
                                  node.location.line, node.location.column);
            }
            const size_t base = environment.workspace.argumentTop;
            if (base + node.children.size() > environment.workspace.arguments.size()) {
                throw SourceError("EXPRESSION_LIMIT", "Nested function argument limit exceeded",
                                  node.location.line, node.location.column);
            }
            for (size_t i = 0; i < node.children.size(); ++i) {
                environment.workspace.argumentTop = base + i + 1;
                environment.workspace.arguments[base + i] =
                    evaluate(*node.children[i], environment, budget);
            }
            const Value result = callBuiltin(node.builtin, node.text,
                environment.workspace.arguments.data() + base,
                node.children.size(), node.location);
            environment.workspace.argumentTop = base;
            return result;
        }
        default: break;
    }
    throw SourceError("RUNTIME_ERROR", "Invalid expression",
                      node.location.line, node.location.column);
}

Value applyAssignment(char operation, const Value& previous, const Value& value) {
    if (operation == '=') return value;
    if (operation == '+') return numericOrStringPlus(previous, value);
    const float left = numberOf(previous);
    const float right = numberOf(value);
    if (operation == '-') return Value::fromNumber(left - right);
    if (operation == '*') return Value::fromNumber(left * right);
    if (operation == '/') return Value::fromNumber(right == 0 ? 0 : left / right);
    return value;
}

struct ExecResult {
    bool returned = false;
    Value value;
};

ExecResult execute(const Node& node, Environment& environment, Budget& budget);

ExecResult executeBlock(const Node& block, Environment& environment, Budget& budget) {
    for (const NodePtr& statement : block.children) {
        ExecResult result = execute(*statement, environment, budget);
        if (result.returned) return result;
    }
    return {};
}

ExecResult execute(const Node& node, Environment& environment, Budget& budget) {
    budget.consume(node.location);
    switch (node.kind) {
        case Node::Kind::Block: return executeBlock(node, environment, budget);
        case Node::Kind::Let:
            environment.workspace.setLocal(node.text,
                evaluate(*node.children[0], environment, budget), node.location);
            return {};
        case Node::Kind::Return:
            return {true, evaluate(*node.children[0], environment, budget)};
        case Node::Kind::Expression:
            evaluate(*node.children[0], environment, budget);
            return {};
        case Node::Kind::Assign: {
            const Node& target = *node.children[0];
            const Value value = evaluate(*node.children[1], environment, budget);
            if (target.kind == Node::Kind::Variable) {
                const Value previous = resolveVariable(target.text, environment, target.location);
                environment.workspace.setLocal(target.text,
                    applyAssignment(node.operation, previous, value), target.location);
                return {};
            }
            if (target.kind == Node::Kind::Index) {
                const Value base = evaluate(*target.children[0], environment, budget);
                const float indexValue = std::floor(numberOf(
                    evaluate(*target.children[1], environment, budget)));
                if (!isArrayValue(base) || !std::isfinite(indexValue) ||
                    indexValue < 0 || indexValue >= static_cast<float>(arrayLength(base))) {
                    throw SourceError("ARRAY_BOUNDS", "Array index out of bounds",
                                      target.location.line, target.location.column);
                }
                const size_t index = static_cast<size_t>(indexValue);
                Value result = applyAssignment(node.operation, arrayElement(base, index), value);
                setArrayElement(base, index, result);
                return {};
            }
            throw SourceError("INVALID_ASSIGNMENT", "Invalid assignment target",
                              target.location.line, target.location.column);
        }
        case Node::Kind::If: {
            const Value condition = evaluate(*node.children[0], environment, budget);
            return executeBlock(*node.children[truthy(condition) ? 1 : 2], environment, budget);
        }
        case Node::Kind::For: {
            environment.workspace.setLocal(node.text,
                evaluate(*node.children[0], environment, budget), node.location);
            uint16_t iterations = 0;
            while (truthy(evaluate(*node.children[1], environment, budget))) {
                if (++iterations > config::MAX_LOOP_ITERATIONS) {
                    throw SourceError("LOOP_LIMIT", "Loop iteration limit exceeded",
                                      node.location.line, node.location.column);
                }
                ExecResult result = executeBlock(*node.children[3], environment, budget);
                if (result.returned) return result;
                const Value oldValue = resolveVariable(node.auxiliary, environment, node.location);
                const Value step = evaluate(*node.children[2], environment, budget);
                environment.workspace.setLocal(node.auxiliary,
                    applyAssignment(node.operation, oldValue, step), node.location);
            }
            return {};
        }
        default:
            throw SourceError("RUNTIME_ERROR", "Invalid statement",
                              node.location.line, node.location.column);
    }
}

RuntimeError convertError(const SourceError& error) {
    return RuntimeError{error.code, error.what(), error.line, error.column};
}

}  // namespace

struct AnimationRuntime::Program {
    NodePtr ast;
    std::unordered_map<std::string, Value> state;
    Workspace workspace;
    std::string name;
    Node* update = nullptr;
    Node* render = nullptr;
};

AnimationRuntime::AnimationRuntime() = default;
AnimationRuntime::~AnimationRuntime() = default;

bool AnimationRuntime::queueCompile(const std::string& name, const std::string& source,
                                    uint32_t& jobId, RuntimeError& error) {
    if (name.size() > config::MAX_ANIMATION_NAME_BYTES) {
        error = RuntimeError{"NAME_TOO_LONG", "Animation name exceeds 64 bytes", 0, 0};
        return false;
    }
    if (source.empty() || source.size() > config::MAX_SOURCE_BYTES) {
        error = RuntimeError{"SOURCE_SIZE", "Animation source must be 1–16384 bytes", 0, 0};
        return false;
    }
    pendingName_ = name;
    pendingSource_ = source;
    jobId_ = nextJobId_++;
    job_ = RuntimeJob{};
    job_.id = jobId_;
    job_.pending = true;
    job_.name = name;
    jobId = job_.id;
    return true;
}

void AnimationRuntime::serviceCompile() {
    if (!job_.pending) return;
    job_.pending = false;
    const uint32_t compileStarted = micros();
    try {
        auto candidate = std::make_unique<Program>();
        std::vector<Token> tokens = Lexer(pendingSource_).tokenize();
        if (micros() - compileStarted > config::MAX_COMPILATION_TIME_US) {
            throw SourceError("COMPILE_TIME_LIMIT", "Compilation time limit exceeded");
        }
        candidate->ast = Parser(std::move(tokens)).parse();
        if (micros() - compileStarted > config::MAX_COMPILATION_TIME_US) {
            throw SourceError("COMPILE_TIME_LIMIT", "Compilation time limit exceeded");
        }
        candidate->name = pendingName_.empty() ? candidate->ast->text : pendingName_;
        candidate->state.reserve(config::MAX_STATE_VARIABLES);
        const std::unordered_map<std::string, Value> emptyState;
        for (const NodePtr& declaration : candidate->ast->children) {
            if (declaration->kind == Node::Kind::State) {
                if (candidate->state.size() >= config::MAX_STATE_VARIABLES) {
                    throw SourceError("STATE_LIMIT", "State variable limit exceeded",
                                      declaration->location.line, declaration->location.column);
                }
                candidate->workspace.beginScope();
                Environment environment{emptyState, candidate->workspace, 0};
                Budget budget{config::MAX_INIT_INSTRUCTIONS,
                              micros() + config::MAX_FRAME_TIME_US};
                Value initial = evaluate(*declaration->children[0], environment, budget);
                candidate->state[declaration->text] = std::move(initial);
            } else if (declaration->kind == Node::Kind::Update) {
                candidate->update = declaration.get();
            } else if (declaration->kind == Node::Kind::Render) {
                candidate->render = declaration.get();
            }
            if (micros() - compileStarted > config::MAX_COMPILATION_TIME_US) {
                throw SourceError("COMPILE_TIME_LIMIT", "Compilation time limit exceeded",
                                  declaration->location.line, declaration->location.column);
            }
        }
        active_ = std::move(candidate);
        simulationTime_ = 0;
        frameCount_ = 0;
        running_ = true;
        job_.complete = true;
        job_.success = true;
        job_.name = active_->name;
    } catch (const SourceError& error) {
        job_.complete = true;
        job_.success = false;
        job_.error = convertError(error);
    } catch (const std::bad_alloc&) {
        job_.complete = true;
        job_.success = false;
        const uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        const uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        job_.error = RuntimeError{
            "OUT_OF_MEMORY",
            "Not enough memory to compile animation (free " +
                std::to_string(freeHeap) + " B, largest block " +
                std::to_string(largestBlock) + " B)",
            0, 0};
    }
    pendingSource_.clear();
    pendingName_.clear();
}

bool AnimationRuntime::renderFrame(float dt, std::array<Rgb, config::LED_COUNT>& output,
                                   RuntimeError& error, bool singleStep) {
    if (!active_ || (!running_ && !singleStep)) {
        error = RuntimeError{"NOT_RUNNING", "No animation is running", 0, 0};
        return false;
    }
    const uint32_t started = micros();
    const uint32_t deadline = started + config::MAX_FRAME_TIME_US;
    workingFrame_.fill(Rgb{});
    simulationTime_ += std::max(0.0f, std::min(dt, 0.05f)) * timeScale_;
    Budget budget{config::MAX_INSTRUCTIONS_PER_FRAME, deadline};
    try {
        if (active_->update) {
            active_->workspace.beginScope();
            Environment environment{active_->state, active_->workspace, simulationTime_};
            active_->workspace.setLocal(active_->update->text, Value::fromNumber(dt),
                                        active_->update->location);
            ExecResult result = execute(*active_->update->children[0], environment, budget);
            if (result.returned) {
                throw SourceError("INVALID_RETURN", "return is not allowed in update()",
                                  active_->update->location.line, active_->update->location.column);
            }
            for (const auto& state : active_->state) {
                const Value* updated = active_->workspace.findLocal(state.first);
                if (updated) active_->state[state.first] = *updated;
            }
        }
        for (uint16_t i = 0; i < config::LED_COUNT; ++i) {
            if ((i & 7U) == 0U && static_cast<int32_t>(micros() - deadline) >= 0) {
                throw SourceError("TIME_LIMIT", "Frame execution time limit exceeded",
                                  active_->render->location.line, active_->render->location.column);
            }
            active_->workspace.beginScope();
            Environment environment{active_->state, active_->workspace, simulationTime_};
            active_->workspace.setLocal(active_->render->text, Value::fromNumber(i),
                                        active_->render->location);
            ExecResult result = execute(*active_->render->children[0], environment, budget);
            if (!result.returned ||
                !(result.value.kind == Value::Kind::Color ||
                  (isArrayValue(result.value) && arrayLength(result.value) >= 3))) {
                throw SourceError("INVALID_COLOR", "render(i) must return rgb(...) or hsv(...)",
                                  active_->render->location.line, active_->render->location.column);
            }
            const auto channel = [&result](size_t index) {
                const float value = result.value.kind == Value::Kind::Color
                                         ? result.value.color[index]
                                         : numberOf(arrayElement(result.value, index));
                return static_cast<float>(std::isfinite(value) ? clamped(value, 0, 1) : 0);
            };
            workingFrame_[i] = Rgb{channel(0), channel(1), channel(2)};
        }
        output = workingFrame_;
        ++frameCount_;
        lastComputeTimeUs_ = micros() - started;
        return true;
    } catch (const SourceError& runtimeError) {
        error = convertError(runtimeError);
    } catch (const std::bad_alloc&) {
        error = RuntimeError{"OUT_OF_MEMORY", "Runtime ran out of memory", 0, 0};
    }
    lastComputeTimeUs_ = micros() - started;
    running_ = false;
    return false;
}

void AnimationRuntime::stop() {
    running_ = false;
}

const std::string& AnimationRuntime::animationName() const {
    static const std::string empty;
    return active_ ? active_->name : empty;
}

}  // namespace ledlab
