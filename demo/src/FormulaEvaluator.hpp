#pragma once

#include <cctype>
#include <stdexcept>
#include <string>

// Minimal recursive-descent evaluator for pure numeric "+ - * / ( )" C
// expressions, used after counters have already been substituted by value.
namespace detail {

class FormulaEvaluator {
public:
    explicit FormulaEvaluator(const std::string& expression) : expression_(expression), pos_(0) {}

    double evaluate() {
        double result = parseExpr();
        skipSpaces();
        if (pos_ != expression_.size()) {
            throw std::runtime_error("Unexpected character in expression: " + expression_.substr(pos_));
        }
        return result;
    }

private:
    const std::string& expression_;
    size_t pos_;

    void skipSpaces() {
        while (pos_ < expression_.size() && std::isspace(static_cast<unsigned char>(expression_[pos_]))) {
            ++pos_;
        }
    }

    char peek() {
        skipSpaces();
        return pos_ < expression_.size() ? expression_[pos_] : '\0';
    }

    double parseExpr() {
        double value = parseTerm();
        for (char c = peek(); c == '+' || c == '-'; c = peek()) {
            ++pos_;
            double rhs = parseTerm();
            value = (c == '+') ? value + rhs : value - rhs;
        }
        return value;
    }

    double parseTerm() {
        double value = parseFactor();
        for (char c = peek(); c == '*' || c == '/'; c = peek()) {
            ++pos_;
            double rhs = parseFactor();
            if (c == '*') {
                value *= rhs;
            } else {
                if (rhs == 0.0) {
                    throw std::runtime_error("Division by zero in expression: " + expression_);
                }
                value /= rhs;
            }
        }
        return value;
    }

    double parseFactor() {
        char c = peek();
        if (c == '+') {
            ++pos_;
            return parseFactor();
        }
        if (c == '-') {
            ++pos_;
            return -parseFactor();
        }
        if (c == '(') {
            ++pos_;
            double value = parseExpr();
            if (peek() != ')') {
                throw std::runtime_error("Missing closing parenthesis in expression: " + expression_);
            }
            ++pos_;
            return value;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            return parseNumber();
        }
        throw std::runtime_error("Unexpected token in expression: " + expression_.substr(pos_));
    }

    double parseNumber() {
        skipSpaces();
        size_t start = pos_;
        while (pos_ < expression_.size() &&
               (std::isdigit(static_cast<unsigned char>(expression_[pos_])) || expression_[pos_] == '.')) {
            ++pos_;
        }
        return std::stod(expression_.substr(start, pos_ - start));
    }
};

}  // namespace detail
