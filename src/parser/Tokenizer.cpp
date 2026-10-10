#include "parser/Tokenizer.h"

#include <cctype>
#include <cstdlib>

namespace thdic {
namespace parser {
namespace {

constexpr int kEof = std::char_traits<char>::eof();

}  // namespace

Tokenizer::Tokenizer(std::istream& in) : in_(in) {}

int Tokenizer::get() {
    for (;;) {
        int c = in_.get();
        if (c == '#') {  // 注释到行尾
            while (c != '\n' && c != kEof) c = in_.get();
            continue;
        }
        return c;
    }
}

bool Tokenizer::readToken(Token& t) {
    int c = get();
    while (c != kEof && std::isspace(static_cast<unsigned char>(c))) c = get();

    t = Token{};
    if (c == kEof) {
        t.type = TokType::End;
        return false;
    }
    if (c == ';' || c == '(' || c == ')') {
        t.type = TokType::Char;
        t.text.assign(1, static_cast<char>(c));
        return true;
    }

    // 首字符是数字则按 Number；负数等交给 tokenToNumber 统一处理。
    t.type =
        std::isdigit(static_cast<unsigned char>(c)) ? TokType::Number : TokType::Ident;
    while (c != kEof && !std::isspace(static_cast<unsigned char>(c)) &&
           c != ';' && c != '(' && c != ')') {
        t.text.push_back(static_cast<char>(c));
        c = get();
    }
    if (c == ';' || c == '(' || c == ')') in_.unget();  // 分隔符留给下一个 token
    return true;
}

bool Tokenizer::next(Token& t) {
    if (hasUnread_) {
        t = unread_;
        hasUnread_ = false;
        return t.type != TokType::End;
    }
    return readToken(t);
}

void Tokenizer::pushBack(const Token& t) {
    if (t.type == TokType::End) return;
    unread_ = t;
    hasUnread_ = true;
}

void Tokenizer::skipToSemicolon() {
    Token t;
    while (next(t)) {
        if (t.type == TokType::Char && t.text == ";") return;
    }
}

bool tokenToNumber(const Token& t, double& value) {
    if (t.text.empty()) return false;
    const char* s = t.text.c_str();
    char* end = nullptr;
    value = std::strtod(s, &end);
    return end != s && *end == '\0';
}

}  // namespace parser
}  // namespace thdic
