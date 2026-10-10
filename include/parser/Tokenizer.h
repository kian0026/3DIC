#pragma once

#include <istream>
#include <string>

namespace thdic {
namespace parser {

enum class TokType { Ident, Number, Char, End };

struct Token {
    TokType type = TokType::End;
    std::string text;
};

// LEF/DEF 共用词法器：只切词，不做语义。
class Tokenizer {
public:
    explicit Tokenizer(std::istream& in);

    bool next(Token& t);            // 取下一个 token，false = EOF
    void pushBack(const Token& t);  // 回退 1 个 token
    void skipToSemicolon();         // 跳到 ';'，错误恢复用

private:
    int get();  // 下一字符，跳过空白与 '#' 注释
    bool readToken(Token& t);

    std::istream& in_;
    bool hasUnread_ = false;
    Token unread_;
};

// 词法上负数（如 -70、-0.035）不是 Number，这里统一转数值。
bool tokenToNumber(const Token& t, double& value);

}  // namespace parser
}  // namespace thdic
