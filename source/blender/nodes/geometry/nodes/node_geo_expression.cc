/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

// TODO
// Fix up execute_program to have a runtime stack class

#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "NOD_geo_Expression.hh"
#include "NOD_node_declaration.hh"
#include "node_geometry_util.hh"

#include "BLI_utildefines.h"
#include "NOD_rna_define.hh"
#include "UI_interface.hh"

#include "NOD_socket_items_ops.hh"
//  #include "UI_resources.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"

#include "BLO_read_write.hh"
#include <charconv>
#include <stdint.h>

#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

namespace blender::nodes::node_geo_expression_cc {

////////////////////////////////////////////////////////////////////////////
// Token
// Struct used for parsing and creating a representation
// of the exprssion for evaluation
////////////////////////////////////////////////////////////////////////////
struct Token {
  enum class TokenType {
    NONE,
    // Constants
    FIRST_CONSTANT,
    CONSTANT_FLOAT = FIRST_CONSTANT,
    CONSTANT_INT,
    // Variables (Inputs)
    FIRST_VARIABLE,
    VARIABLE_FLOAT = FIRST_VARIABLE,
    VARIABLE_INT,
    VARIABLE_BOOL,
    VARIABLE_VEC,
    FIRST_SPECIAL,
    LEFT_PAREN = FIRST_SPECIAL,
    RIGHT_PAREN,
    // Operators
    FIRST_OPERATOR,
    OPERATOR_UNARY_MINUS = FIRST_OPERATOR,
    OPERATOR_UNARY_MINUS_INT,
    OPERATOR_UNARY_MINUS_VEC,
    OPERATOR_PLUS,
    OPERATOR_PLUS_INT,
    OPERATOR_PLUS_VEC,
    OPERATOR_MINUS,
    OPERATOR_MINUS_INT,
    OPERATOR_MINUS_VEC,
    OPERATOR_MULTIPLY,
    OPERATOR_MULTIPLY_INT,
    OPERATOR_MULTIPLY_FLOAT_VEC,
    OPERATOR_MULTIPLY_VEC_FLOAT,
    OPERATOR_DIVIDE,
    OPERATOR_DIVIDE_INT,
    OPERATOR_DIVIDE_VEC_FLOAT,
    OPERATOR_POWER,
    OPERATOR_POWER_INT,
    FIRST_POSTFIX_OPERATOR,
    OPERATOR_GET_MEMBER_VEC = FIRST_POSTFIX_OPERATOR,
    // Functions
    FIRST_FUNCTION,
    FUNCTION_SQUARE_ROOT = FIRST_FUNCTION,
    FUNCTION_SINE,
    FUNCTION_COSINE,
    FUNCTION_MAX,
    FUNCTION_MAX_INT,
    CONVERT_INT_FLOAT,
    CONVERT_FLOAT_INT,
    NUM
  };

  // Describes the type of argument on the stack
  // We keep track of types while creating the program
  // issuing modified token types to always use the correct types
  // so that no checking is required during evaluation
  enum class eValueType { NONE, FLOAT, INT, VEC, NUM };

  // Use some abreviations otherwise format on save messes up format

  TokenType type;
  int value;

  // constructors
 public:
  inline Token() : type(TokenType::NONE), value(0) {}
  inline Token(TokenType t, int param) : type(t), value(param) {}
  inline Token(TokenType t, float param) : type(t)
  {
    // Store the float in the int space
    auto fPtr = reinterpret_cast<float *>(&value);
    *fPtr = param;
  }

  inline Token(const Token &other) : type(other.type), value(other.value) {}

  inline bool is_operand() const
  {
    return type >= TokenType::FIRST_CONSTANT && type < TokenType::FIRST_SPECIAL;
  }
  inline bool is_constant() const
  {
    return type >= TokenType::FIRST_CONSTANT && type < TokenType::FIRST_VARIABLE;
  }
  inline bool is_operator() const
  {
    return type >= TokenType::FIRST_OPERATOR && type < TokenType::FIRST_FUNCTION;
  }
  inline bool is_operator_or_function() const
  {
    return type >= TokenType::FIRST_OPERATOR && type < TokenType::NUM;
  }
  inline bool is_postfix_operator() const
  {
    return type >= TokenType::FIRST_POSTFIX_OPERATOR && type < TokenType::NUM;
  }

  inline float get_value_as_float() const
  {
    return *reinterpret_cast<const float *>(&value);
  };

  // Some accessor functions for token info
  inline int precedence() const;
  inline int num_args() const;
  inline eValueType result_type() const;
  static inline eValueType result_type(TokenType t);
};

////////////////////////////////////////////////////////////////////////////
// TokenInfo
// Information about various tokens
////////////////////////////////////////////////////////////////////////////
struct TokenInfo {
  Token::TokenType type;
  const char *name;
  Token::eValueType result_type;
  int num_args;
  Token::eValueType arg1_type;
  Token::eValueType arg2_type;
  int precedence;
};

// Need some abbreviations to stop lines getting too long
// and the auto formatter messing things up
using EV = Token::eValueType;
using T = Token::TokenType;

static const TokenInfo const token_info[(int)T::NUM] = {
    {T::NONE, "NONE", EV::NONE, 0, EV::NONE, EV::NONE},
    // Constants
    {T::CONSTANT_FLOAT, "CONST_FLOAT", EV::FLOAT, 0, EV::NONE, EV::NONE, 0},
    {T::CONSTANT_INT, "CONSTANT_INT", EV::INT, 0, EV::NONE, EV::NONE, 0},
    // Variables (Inputs)
    {T::VARIABLE_FLOAT, "VARIABLE_FLOAT", EV::FLOAT, 0, EV::NONE, EV::NONE, 0},
    {T::VARIABLE_INT, "VARIABLE_INT", EV::INT, 0, EV::NONE, EV::NONE, 0},
    {T::VARIABLE_BOOL, "VARIABLE_BOOL", EV::INT, 0, EV::NONE, EV::NONE, 0},
    {T::VARIABLE_VEC, "VARIABLE_VECTOR", EV::VEC, 0, EV::NONE, EV::NONE, 0},
    // Specials
    {T::LEFT_PAREN, "LEFT_PAREN", EV::NONE, 0, EV::NONE, EV::NONE, 0},
    {T::RIGHT_PAREN, "RIGHT_PAREN", EV::NONE, 0, EV::NONE, EV::NONE, 0},
    // Operators
    {T::OPERATOR_UNARY_MINUS, "OP_UNARY_MINUS_F", EV::FLOAT, 1, EV::FLOAT, EV::NONE, 7},
    {T::OPERATOR_UNARY_MINUS_INT, "OP_UNARY_MINUS_I", EV::INT, 1, EV::INT, EV::NONE, 7},
    {T::OPERATOR_UNARY_MINUS_VEC, "OP_UNARY_MINUS_V", EV::VEC, 1, EV::VEC, EV::NONE, 7},
    {T::OPERATOR_PLUS, "OP_PLUS_F", EV::FLOAT, 2, EV::FLOAT, EV::FLOAT, 1},
    {T::OPERATOR_PLUS_INT, "OP_PLUS_I", EV::INT, 2, EV::INT, EV::INT, 1},
    {T::OPERATOR_PLUS_VEC, "OP_PLUS_V", EV::VEC, 2, EV::VEC, EV::VEC, 1},
    {T::OPERATOR_MINUS, "OP_MINUS_F", EV::FLOAT, 2, EV::FLOAT, EV::FLOAT, 1},
    {T::OPERATOR_MINUS_INT, "OP_MINUS_I", EV::INT, 2, EV::INT, EV::INT, 1},
    {T::OPERATOR_MINUS_VEC, "OP_MINUS_V", EV::VEC, 2, EV::VEC, EV::VEC, 1},
    {T::OPERATOR_MULTIPLY, "OP_MULTIPLY_F", EV::FLOAT, 2, EV::FLOAT, EV::FLOAT, 2},
    {T::OPERATOR_MULTIPLY_INT, "OP_MULTIPLY_I", EV::INT, 2, EV::INT, EV::INT, 2},
    {T::OPERATOR_MULTIPLY_FLOAT_VEC, "OP_MULTIPLY_FV", EV::VEC, 2, EV::FLOAT, EV::VEC, 2},
    {T::OPERATOR_MULTIPLY_VEC_FLOAT, "OP_MULTIPLY_VF", EV::VEC, 2, EV::VEC, EV::FLOAT, 2},
    {T::OPERATOR_DIVIDE, "OP_DIVIDE_F", EV::FLOAT, 2, EV::FLOAT, EV::FLOAT, 2},
    {T::OPERATOR_DIVIDE_INT, "OP_DIVIDE_I", EV::INT, 2, EV::INT, EV::INT, 2},
    {T::OPERATOR_DIVIDE_VEC_FLOAT, "OP_DIVIDE_VF", EV::VEC, 2, EV::VEC, EV::FLOAT, 2},
    {T::OPERATOR_POWER, "OP_POWER_F", EV::FLOAT, 2, EV::FLOAT, EV::FLOAT, 8},
    {T::OPERATOR_POWER_INT, "OP_POWER_I", EV::INT, 2, EV::INT, EV::INT, 8},
    {T::OPERATOR_GET_MEMBER_VEC, "OP_READ_MEMBER_V", EV::FLOAT, 1, EV::VEC, EV::NONE, 7},
    // Functions
    {T::FUNCTION_SQUARE_ROOT, "FN_SQUARE_ROOT", EV::FLOAT, 1, EV::FLOAT, EV::NONE, 9},
    {T::FUNCTION_SINE, "FN_SIN", EV::FLOAT, 1, EV::FLOAT, EV::NONE, 9},
    {T::FUNCTION_COSINE, "FN_COS", EV::FLOAT, 1, EV::FLOAT, EV::NONE, 9},
    {T::FUNCTION_MAX, "FN_MAX_F", EV::FLOAT, 2, EV::FLOAT, EV::FLOAT, 9},
    {T::FUNCTION_MAX_INT, "FN_MAX_I", EV::INT, 2, EV::INT, EV::INT, 9},
    {T::CONVERT_INT_FLOAT, "FN_CONV_I2F", EV::FLOAT, 1, EV::INT, EV::NONE, 9},
    {T::CONVERT_FLOAT_INT, "FN_CONV_F2I", EV::INT, 1, EV::FLOAT, EV::NONE, 9},
};

inline int Token::precedence() const
{
  return token_info[(int)type].precedence;
}

inline int Token::num_args() const
{
  return token_info[(int)type].num_args;
}

inline Token::eValueType Token::result_type() const
{
  return token_info[(int)type].result_type;
}

inline Token::eValueType Token::result_type(TokenType t)
{
  return token_info[(int)t].result_type;
}

#ifndef NDEBUG
// Check that token info is correctly size and has entries in correct order
void token_info_check()
{
  BLI_assert(sizeof(token_info) / sizeof(TokenInfo) == (int)T::NUM);
  for (int t = (int)Token::TokenType::NONE; t < (int)Token::TokenType::NUM; t++)
    BLI_assert(token_info[t].type == (Token::TokenType)t);
}
#endif

////////////////////////////////////////////////////////////////////////////
// TokenQueue
// Class that maintains an array of tokens
////////////////////////////////////////////////////////////////////////////
class TokenQueue {
  Vector<Token, 50, GuardedAllocator> buffer_{};

 public:
  TokenQueue() {}

  void add_token(Token::TokenType t, int param)
  {
    buffer_.append(Token(t, param));
  }
  void add_token(Token::TokenType t, float param)
  {
    buffer_.append(Token(t, param));
  }

  void add_token(Token t)
  {
    buffer_.append(t);
  }

  const int element_count() const
  {
    return buffer_.size();
  }

  Token at(int index) const
  {
    return buffer_[index];
  }

  void clear()
  {
    buffer_.clear();
  }

  const bool is_empty() const
  {
    return buffer_.is_empty();
  }

  void discard_last()
  {
    buffer_.pop_last();
  }

  Token last() const
  {
    return buffer_.last();
  }

  void print() const
  {
    printf("%i Tokens:\n", buffer_.size());
    for (int i = 0; i < buffer_.size(); i++) {
      Token t = buffer_[i];
      if (t.is_operand())
        printf("%s(%d) ", token_info[(int)t.type].name, t.value);
      else
        printf("%s ", token_info[(int)t.type].name);

      if (i > 0 && (i % 8) == 0)
        printf("\n");
    }
    printf("\n");
  }
};

////////////////////////////////////////////////////////////////////////////
// ExpressionParser
// Class that parses the expression into a buffer of tokens
////////////////////////////////////////////////////////////////////////////
class ExpressionParser {
 public:
  const Vector<const char *> *input_names_;
  const Vector<short> *input_types_;
  const char *error_msg_ = "";
  int error_pos_ = -1;

  ExpressionParser(const Vector<const char *> *input_names, const Vector<short> *input_types)
      : input_names_(input_names), input_types_(input_types)
  {
  }

  bool parse(const char *expression, TokenQueue &buffer, const char *&error_msg, int &error_pos)
  {
    error_msg_ = "";
    error_pos_ = 0;

    int read_pos = 0;
    bool ok = parse_expression(expression, read_pos, buffer, false);
    error_msg = error_msg_;
    error_pos = error_pos_;
    return ok;
  }

  void set_error_if_none(const char *msg, int position)
  {
    if (error_msg_ == nullptr || error_msg_ == "") {
      error_msg_ = msg;
      error_pos_ = position;
    }
  }

  bool parse_expression(const std::string &input,
                        int &read_pos,
                        TokenQueue &output,
                        bool terminate_on_close_parens)
  {
    skip_white_space(input, read_pos);
    if (read_pos == input.length())
      return false;

    if (!parse_operand_or_unary(input, read_pos, output)) {
      set_error_if_none("Expected an operand", read_pos);
      return false;
    }

    while (true) {
      // If we've reached the end of the input or a parenthesized expression
      // then we have a valid exprssion
      skip_white_space(input, read_pos);
      if (read_pos == input.length())
        return true;
      if (terminate_on_close_parens && input.at(read_pos) == ')')
        return true;

      // Expect an operator and another operand
      if (!parse_operator(input, read_pos, output)) {
        set_error_if_none("Expected an operator", read_pos);
        return false;
      }
      // expect another operand after an operator, unless it was postifx
      if (!output.last().is_postfix_operator()) {
        if (!parse_operand_or_unary(input, read_pos, output)) {
          set_error_if_none("Expected an operand after operator", read_pos);
          return false;
        }
      }
    }

    return true;
  }

  bool parse_operand_or_unary(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);
    if (read_pos == input.length())
      return false;

    // Check for unary minus operator. Skip if followed by digit
    if (input.at(read_pos) == '-' && read_pos < input.length() - 1 &&
        !isdigit(input.at(read_pos + 1)))
    {
      output.add_token(Token::TokenType::OPERATOR_UNARY_MINUS, 0);
      read_pos++;
      if (!parse_operand(input, read_pos, output)) {
        set_error_if_none("Expected operand after unary operator", read_pos);
        return false;
      }
    }
    else
      return parse_operand(input, read_pos, output);
  }

  bool parse_operand(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      return false;

    if (input.at(read_pos) == '(') {
      int paren_start = read_pos;
      output.add_token(Token::TokenType::LEFT_PAREN, 0);
      read_pos++;

      if (!parse_expression(input, read_pos, output, true)) {
        set_error_if_none("Expected expression after parenthesis", read_pos);
        return false;
      }

      if (!parse_right_paren(input, read_pos, output)) {
        error_msg_ = "Unclosed parenthesis";
        error_pos_ = paren_start;
        return false;
      }

      return true;
    }
    else {
      if (next_input_is_function_name(input, read_pos))
        return parse_function(input, read_pos, output);
      if (parse_number(input, read_pos, output))
        return true;
      error_msg_ = "";  // discard error message from attempting to read number
      if (read_variable_name_size(input, read_pos) != 0)
        return parse_variable(input, read_pos, output);

      set_error_if_none("Expected a constant, variable or function", read_pos);
      return false;
    }
  }

  bool parse_function(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      return false;

    int start_read_pos = read_pos;

    // Read the function name
    auto function_op = read_function_op(input, read_pos);
    if (function_op == Token::TokenType::NONE) {
      set_error_if_none("Unknown function name", start_read_pos);
      return false;
    }

    output.add_token(function_op, 0);

    // Now expect a left paren
    if (!parse_left_paren(input, read_pos, output))
      return false;

    // Now expect an expression
    start_read_pos = read_pos;
    if (!parse_expression(input, read_pos, output, true)) {
      set_error_if_none("Expected an expression as function parameter", start_read_pos);
      return false;
    }

    // TODO expect commas and further expressions for multi-operand functions

    // Expect a right param
    if (!parse_right_paren(input, read_pos, output))
      return false;
  }

  bool parse_left_paren(const std::string &input, int &read_pos, TokenQueue &output)
  {
    bool fail = false;

    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      fail = true;

    if (!fail && input.at(read_pos) == '(') {
      output.add_token(Token::TokenType::LEFT_PAREN, 0);
      read_pos++;
      return true;
    }
    else {
      set_error_if_none("Expected (", read_pos);
      return false;
    }
  }
  bool parse_right_paren(const std::string &input, int &read_pos, TokenQueue &output)
  {
    bool fail = false;
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      fail = true;

    if (!fail && input.at(read_pos) == ')') {
      output.add_token(Token::TokenType::RIGHT_PAREN, 0);
      read_pos++;
      return true;
    }
    else {
      set_error_if_none("Expected )", read_pos);
      return false;
    }
  }

  bool parse_operator(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      return false;

    int start_read_pos = read_pos;
    auto op = read_operator_op(input, read_pos);
    if (op == Token::TokenType::NONE) {
      return false;
    }

    if (op == Token::TokenType::OPERATOR_GET_MEMBER_VEC) {
      // This op must be followed directly by a field name
      int field_offset = read_member_offset(input, read_pos);
      if (field_offset == -1) {
        read_pos = start_read_pos;
        set_error_if_none("Expected member name directly after \".\"", read_pos);
        return false;
      }
      output.add_token(op, field_offset);
    }
    else
      output.add_token(op, 0);

    return true;
  }

  bool parse_number(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      return false;

    auto sub_string = input.substr(read_pos, input.length() - read_pos);
    auto last = sub_string.data() + sub_string.size();

    // See if we can read it as either a float or an int, and pick whichever uses more characters
    float f = 0.0;
    auto [fptr, fec] = std::from_chars(sub_string.data(), last, f);
    int x = 0;
    auto [iptr, iec] = std::from_chars(sub_string.data(), last, x);

    if (iec == std::errc{} || fec == std::errc{}) {
      const char *ptr;
      if (iec == std::errc{} && (fec != std::errc{} || iptr >= fptr)) {
        output.add_token(Token::TokenType::CONSTANT_INT, x);
        ptr = iptr;
      }
      else {
        output.add_token(Token::TokenType::CONSTANT_FLOAT, f);
        ptr = fptr;
      }
      if (ptr == last)
        read_pos += sub_string.length();
      else
        read_pos += ptr - sub_string.data();

      return true;
    }

    set_error_if_none("Invalid number", read_pos);
    return false;
  }

  bool parse_variable(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      return false;

    int name_len = read_variable_name_size(input, read_pos);
    if (name_len == 0) {
      set_error_if_none("Expected a variable name", read_pos);
      return false;
    }
    std::string var_name = input.substr(read_pos, name_len);

    // Check that variable actually exists
    int input_idx = -1;
    for (int i = 0; i < input_names_->size(); i++) {
      if ((*input_names_)[i] == var_name) {
        input_idx = i;
        break;
      }
    }

    if (input_idx == -1) {
      set_error_if_none("Unknown input name", read_pos);
      return false;
    }

    read_pos += name_len;
    switch ((*input_types_)[input_idx]) {
      case eNodeSocketDatatype::SOCK_BOOLEAN:
        output.add_token(Token::TokenType::VARIABLE_BOOL, input_idx);
        break;
      case eNodeSocketDatatype::SOCK_INT:
        output.add_token(Token::TokenType::VARIABLE_INT, input_idx);
        break;
      case eNodeSocketDatatype::SOCK_FLOAT:
        output.add_token(Token::TokenType::VARIABLE_FLOAT, input_idx);
        break;
      case eNodeSocketDatatype::SOCK_VECTOR:
        output.add_token(Token::TokenType::VARIABLE_VEC, input_idx);
        break;
      default:
        BLI_assert_unreachable();
    }
    return true;
  }

  Token::TokenType read_operator_op(const std::string &input, int &read_pos)
  {
    skip_white_space(input, read_pos);
    if (read_pos == input.length())
      return Token::TokenType::NONE;

    if (input.at(read_pos) == '+') {
      read_pos++;
      return Token::TokenType::OPERATOR_PLUS;
    }
    if (input.at(read_pos) == '-') {
      read_pos++;
      return Token::TokenType::OPERATOR_MINUS;
    }
    if (input.at(read_pos) == '*') {
      read_pos++;
      return Token::TokenType::OPERATOR_MULTIPLY;
    }
    if (input.at(read_pos) == '/') {
      read_pos++;
      return Token::TokenType::OPERATOR_DIVIDE;
    }
    if (input.at(read_pos) == '.') {
      read_pos++;
      return Token::TokenType::OPERATOR_GET_MEMBER_VEC;
    }

    return Token::TokenType::NONE;
  }

  int read_member_offset(const std::string &input, int &read_pos)
  {
    // Note, don't skip whitespace
    if (read_pos == input.length())
      return -1;

    const char name = input.at(read_pos);
    read_pos++;
    if (name == 'x' || name == 'X')
      return 2;
    if (name == 'y' || name == 'Y')
      return 1;
    if (name == 'z' || name == 'Z')
      return 0;

    read_pos--;  // restore read pos before returning error
    return -1;
  }

  bool next_input_is_function_name(const std::string &input, int read_pos)
  {
    int temp_read_pos = read_pos;
    return read_function_op(input, temp_read_pos) != Token::TokenType::NONE;
  }

  Token::TokenType read_function_op(const std::string &input, int &read_pos)
  {
    skip_white_space(input, read_pos);
    if (input.find("Sin", read_pos) == read_pos || input.find("sin", read_pos) == read_pos) {
      read_pos += 3;
      return Token::TokenType::FUNCTION_SINE;
    }
    if (input.find("Cos", read_pos) == read_pos || input.find("cos", read_pos) == read_pos) {
      read_pos += 3;
      return Token::TokenType::FUNCTION_COSINE;
    }
    if (input.find("Sqrt", read_pos) == read_pos || input.find("sqrt", read_pos) == read_pos) {
      read_pos += 4;
      return Token::TokenType::FUNCTION_SQUARE_ROOT;
    }

    return Token::TokenType::NONE;
  }

  void skip_white_space(const std::string &input, int &read_pos) const
  {
    while (read_pos < input.length() && isspace(input.at(read_pos)))
      read_pos++;
  }

  // Returns the number of characters that constitute a valid variable name
  // Doesn't adjust read_pos
  // Only checks for a syntacically valid name. Doesn't check if such a variable exists
  // returns 0 if no valid name
  int read_variable_name_size(const std::string &input, int &read_pos) const
  {
    int temp_read_pos = read_pos;
    skip_white_space(input, temp_read_pos);
    if (temp_read_pos == input.length())
      return 0;

    // Check if the first character is valid
    char first = input.at(temp_read_pos++);
    if (first != '_' && !isalpha(first))
      return 0;

    while (temp_read_pos < input.length()) {
      char c = input.at(temp_read_pos);
      if (c != '_' && !isalpha(c) && !isdigit(c))
        break;
      temp_read_pos++;
    }

    return temp_read_pos - read_pos;
  }
};

////////////////////////////////////////////////////////////////////////////
// ExpressionProgram
// class that holds a representation of the expression for evaluation
// Creates and evaluates the representation
////////////////////////////////////////////////////////////////////////////
class ExpressionProgram {

  using TokenType = Token::TokenType;
  using eValueType = Token::eValueType;

  bool program_valid_ = false;
  TokenQueue program_buffer_;
  static constexpr int MAX_STACK = 100;

 public:
  const Vector<const char *> *input_names_;
  const Vector<short> *input_types_;
  eNodeSocketDatatype output_type_;

 public:
  ExpressionProgram(const Vector<const char *> *input_names,
                    const Vector<short> *input_types,
                    eNodeSocketDatatype output_type)
      : input_names_(input_names), input_types_(input_types), output_type_(output_type)
  {
  }

  bool create_program(const std::string &expression, std::string &error_msg)
  {
    program_valid_ = false;

    // Try to parse the expression
    TokenQueue parse_buffer;
    ExpressionParser parser(input_names_, input_types_);
    const char *parser_error;
    int error_pos;
    bool ok = parser.parse(expression.c_str(), parse_buffer, parser_error, error_pos);

    // Report parsing errors
    if (!ok) {
      // Combine the error message with part of the expression from the error location to give
      // final message
      error_msg = std::move(std::string(TIP_(parser_error)));
      int chars_after_error = expression.size() - error_pos;
      if (chars_after_error == 0 && error_pos > 0) {
        error_pos--;
        chars_after_error++;
      }
      auto expPart = expression.substr(error_pos, chars_after_error);
      error_msg += "\n" + expPart;
      goto exit;
    }

    // Rearrange the raw parsed tokens into a program that can be evaluated
    if (!create_postfix_program(parse_buffer, program_buffer_, error_msg)) {
      goto exit;
    }

    program_valid_ = true;
    goto exit;

  exit:
    // These don't need to persist after this method has run
    input_names_ = nullptr;
    input_types_ = nullptr;
    return program_valid_;
  }

  inline int stack_space(eValueType type)
  {
    return type == eValueType::VEC ? 3 : 1;
  }

  // Find the correct operator or function token for a particular argument type
  static TokenType get_op_version_for_type(TokenType base_type, eValueType arg_type)
  {
    if (base_type == TokenType::OPERATOR_GET_MEMBER_VEC && arg_type == eValueType::VEC)
      return base_type;

    // Most operators and functions default to float args
    // If there are exceptions they should be checked before here
    if (arg_type == eValueType::FLOAT)
      return base_type;

    switch (base_type) {
      case TokenType::OPERATOR_UNARY_MINUS:
        if (arg_type == eValueType::VEC)
          return TokenType::OPERATOR_UNARY_MINUS_VEC;
        if (arg_type == eValueType::INT)
          return TokenType::OPERATOR_UNARY_MINUS_INT;
        break;
      case TokenType::OPERATOR_POWER:
        if (arg_type == eValueType::INT)
          return TokenType::OPERATOR_POWER_INT;
        break;
    }

    // No conversion found
    return TokenType::NONE;
  }

  // Find the correct operator or function token for a particular argument pair
  static TokenType get_op_version_for_type(TokenType base_type,
                                           eValueType arg_type1,
                                           eValueType arg_type2)
  {
    // All operators and functions default to float args
    // If there are ever any exceptions they can be checked before here
    if (arg_type1 == eValueType::FLOAT && arg_type2 == eValueType::FLOAT)
      return base_type;

    switch (base_type) {
      case TokenType::OPERATOR_PLUS:
        if (arg_type1 == eValueType::VEC && arg_type2 == eValueType::VEC)
          return TokenType::OPERATOR_PLUS_VEC;
        if (arg_type1 == eValueType::INT && arg_type2 == eValueType::INT)
          return TokenType::OPERATOR_PLUS_INT;
        break;
      case TokenType::OPERATOR_MINUS:
        if (arg_type1 == eValueType::VEC && arg_type2 == eValueType::VEC)
          return TokenType::OPERATOR_MINUS_VEC;
        if (arg_type1 == eValueType::INT && arg_type2 == eValueType::INT)
          return TokenType::OPERATOR_MINUS_INT;
        break;
      case TokenType::OPERATOR_MULTIPLY:
        if (arg_type1 == eValueType::INT && arg_type2 == eValueType::INT)
          return TokenType::OPERATOR_MULTIPLY_INT;
        if (arg_type1 == eValueType::VEC && arg_type2 == eValueType::FLOAT)
          return TokenType::OPERATOR_MULTIPLY_VEC_FLOAT;
        if (arg_type1 == eValueType::FLOAT && arg_type2 == eValueType::VEC)
          return TokenType::OPERATOR_MULTIPLY_FLOAT_VEC;
        break;
      case TokenType::OPERATOR_DIVIDE:
        if (arg_type1 == eValueType::INT && arg_type2 == eValueType::INT)
          return TokenType::OPERATOR_DIVIDE_INT;
        if (arg_type1 == eValueType::VEC && arg_type2 == eValueType::FLOAT)
          return TokenType::OPERATOR_DIVIDE_VEC_FLOAT;
        break;
      case TokenType::FUNCTION_MAX:
        if (arg_type1 == eValueType::INT && arg_type2 == eValueType::INT)
          return TokenType::FUNCTION_MAX_INT;
        break;
    }

    // No conversion found
    return TokenType::NONE;
  }

  // if allowed_implicit_only is true only return conversions op for conversions we want to do
  // implicitly
  TokenType get_type_conversion_op(eValueType from_type,
                                   eValueType to_type,
                                   bool allowed_implicit_only = true)
  {
    if (from_type == eValueType::INT && to_type == eValueType::FLOAT)
      return TokenType::CONVERT_INT_FLOAT;

    // No more implicit conversions
    if (allowed_implicit_only)
      return TokenType::NONE;

    if (from_type == eValueType::FLOAT && to_type == eValueType::INT)
      return TokenType::CONVERT_FLOAT_INT;

    // No suitable conversion
    return TokenType::NONE;
  }

  void output_constant(const Token &t,
                       TokenQueue &output,
                       Vector<Token::eValueType> &stack_type,
                       int &stack_size)
  {
    output.add_token(t);
    stack_size++;  // constants are ints or floats
    if (t.type == Token::TokenType::CONSTANT_FLOAT)
      stack_type.append(eValueType::FLOAT);
    else
      stack_type.append(eValueType::INT);
  }

  void output_variable(const Token &t,
                       TokenQueue &output,
                       Vector<eValueType> &stack_type,
                       int &stack_size)
  {
    output.add_token(t);
    if (t.type == Token::TokenType::VARIABLE_VEC) {
      stack_type.append(eValueType::VEC);
    }
    else {
      if (t.type == Token::TokenType::VARIABLE_INT || t.type == Token::TokenType::VARIABLE_BOOL)
        stack_type.append(eValueType::INT);
      else
        stack_type.append(eValueType::FLOAT);
    }

    // Increase stack size by size of type we just added
    stack_size += stack_space(stack_type.last());
  }

  // Checks if the token can operate with the given arg type (returns token if true)
  // Then tries to find a specialized version of the token for the arg type, and returns that
  // If none found, attempts type conversions, pushing necessary conversions ops into the buffer
  // Returns the actual TokenType to use, and sets arg_type to the new arg type
  // Returns TokenType::NONE if no suitable type conversions are available
  TokenType perform_type_conversion(TokenQueue &output, TokenType type, eValueType &arg_type)
  {
    TokenType specialized_op = get_op_version_for_type(type, arg_type);
    if (specialized_op != TokenType::NONE)
      return specialized_op;

    // See if we can convert int to float
    if (arg_type == eValueType::INT) {
      specialized_op = get_op_version_for_type(type, eValueType::FLOAT);
      if (specialized_op != TokenType::NONE) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, 0);  // Insert conversion op
        arg_type = eValueType::FLOAT;                       // arg type has changed
        return specialized_op;
      }
    }

    return TokenType::NONE;
  }

  // As above for two args
  TokenType perform_type_conversion(TokenQueue &output,
                                    TokenType type,
                                    eValueType &arg1_type,
                                    eValueType &arg2_type)
  {
    TokenType specialized_op = get_op_version_for_type(type, arg1_type, arg2_type);
    if (specialized_op != TokenType::NONE)
      return specialized_op;

    // Check if we can convert arg1_type to arg2_type
    TokenType convert_op = get_type_conversion_op(arg1_type, arg2_type);
    if (convert_op != TokenType::NONE) {
      specialized_op = get_op_version_for_type(type, arg2_type, arg2_type);
      if (specialized_op != TokenType::NONE) {
        output.add_token(convert_op,
                         stack_space(arg2_type));  // Convert first arg (1 above stack top)
        arg1_type = arg2_type;                     // set new type
        return specialized_op;
      }
    }

    // Check if we can convert arg2_type to arg1_type
    convert_op = get_type_conversion_op(arg2_type, arg1_type);
    if (convert_op != TokenType::NONE) {
      specialized_op = get_op_version_for_type(type, arg1_type, arg1_type);
      if (specialized_op != TokenType::NONE) {
        output.add_token(convert_op, 0);  // Convert second arg (stack top)
        arg2_type = arg1_type;            // set new type
        return specialized_op;
      }
    }

    // See if we can convert int to float
    if (arg1_type == eValueType::INT && arg2_type == eValueType::INT) {
      TokenType specialized_op = get_op_version_for_type(
          type, eValueType::FLOAT, eValueType::FLOAT);
      if (specialized_op != TokenType::NONE) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, 1);  // Convert arg1
        output.add_token(TokenType::CONVERT_INT_FLOAT, 0);  // Convert arg2
        arg1_type = eValueType::FLOAT;                      // arg type has changed
        arg2_type = eValueType::FLOAT;                      // arg type has changed
        return specialized_op;
      }
    }

    // If we have a vector and an int, try converting int to float
    if (arg1_type == eValueType::INT && arg2_type == eValueType::VEC) {
      TokenType specialized_op = get_op_version_for_type(type, eValueType::FLOAT, arg2_type);
      if (specialized_op != TokenType::NONE) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, stack_space(arg2_type));  // Convert arg1
        arg1_type = eValueType::FLOAT;  // arg type has changed
        return specialized_op;
      }
    }
    if (arg1_type == eValueType::VEC && arg2_type == eValueType::INT) {
      TokenType specialized_op = get_op_version_for_type(type, arg1_type, eValueType::FLOAT);
      if (specialized_op != TokenType::NONE) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, 0);  // Convert arg2
        arg2_type = eValueType::FLOAT;                      // arg type has changed
        return specialized_op;
      }
    }

    return TokenType::NONE;
  }

  bool output_op_or_function(const Token &t,
                             TokenQueue &output,
                             Vector<Token::eValueType> &stack_type,
                             int &stack_size)
  {
    if (t.num_args() == 1) {
      // All operators are parsed as the float type operator
      // Now that we know the actual argument type, get the version for that type
      eValueType arg_type = stack_type.last();
      Token::TokenType specialized_op = perform_type_conversion(output, t.type, arg_type);

      if (specialized_op == TokenType::NONE) {
        return false;
      }

      output.add_token(specialized_op, t.value);

      eValueType result_type = Token::result_type(specialized_op);
      stack_size -= stack_space(arg_type);
      stack_size += stack_space(result_type);
      stack_type.pop_last();
      stack_type.append(result_type);

      return true;
    }

    // Assume we have a two argument operator (ternary ops not supported)
    // Get the arg types and a specialized op for the two types
    eValueType first_type = stack_type.last(1);
    eValueType second_type = stack_type.last(0);
    Token::TokenType specialized_op = perform_type_conversion(
        output, t.type, first_type, second_type);

    if (specialized_op == TokenType::NONE)
      return false;

    output.add_token(specialized_op, t.value);

    eValueType result_type = Token::result_type(specialized_op);
    // Assume that any conversion op doesn't change the amount of stack space used
    stack_size -= stack_space(first_type);
    stack_size -= stack_space(second_type);
    stack_size += stack_space(result_type);
    stack_type.pop_last();  // Remove the types of the arguments
    stack_type.pop_last();
    stack_type.append(result_type);  // and add that of the result

    return true;
  }

  bool push_function() {}

  bool create_postfix_program(TokenQueue const &parse_buffer,
                              TokenQueue &output,
                              std::string &error_msg)
  {
    TokenQueue operator_stack;
    Vector<Token::eValueType> stack_type;
    int stack_size = 0;  // number of float equivilents on stack

    for (int n = 0; n < parse_buffer.element_count(); n++) {
      Token t = parse_buffer.at(n);
      if (t.is_operand()) {
        if (t.is_constant())
          output_constant(t, output, stack_type, stack_size);
        else
          output_variable(t, output, stack_type, stack_size);
      }
      else if (t.is_operator_or_function()) {
        // If this operator has higher precedence than that on the stack,
        // or the stack is empty or contains a paren,
        // then push it onto the stack
        int precedence = t.precedence();
        if (operator_stack.is_empty() ||
            operator_stack.last().type == Token::TokenType::LEFT_PAREN ||
            operator_stack.last().precedence() < precedence)
        {
          operator_stack.add_token(t);
        }
        else {
          // Pop operators with higher or equal precedence off the stack and push them to output
          // then put this token on the stack
          while (!operator_stack.is_empty()) {
            Token top = operator_stack.last();
            if (top.precedence() < precedence || top.type == Token::TokenType::LEFT_PAREN)
              break;
            if (!output_op_or_function(top, output, stack_type, stack_size)) {
              error_msg = unsupported_type_error(top);
              return false;
            }
            operator_stack.discard_last();
          };
          operator_stack.add_token(t);
        }
      }
      else if (t.type == Token::TokenType::LEFT_PAREN) {
        operator_stack.add_token(t);
      }
      else if (t.type == Token::TokenType::RIGHT_PAREN) {
        // Pop operators off the stack until we reach the LEFT_PAREN
        while (operator_stack.last().type != Token::TokenType::LEFT_PAREN) {
          Token top = operator_stack.last();
          if (!output_op_or_function(top, output, stack_type, stack_size)) {
            error_msg = unsupported_type_error(top);
            return false;
          }
          operator_stack.discard_last();
        };
        operator_stack.discard_last();  // discard the left paren
      }

      if (stack_size > MAX_STACK) {
        error_msg = std::string(TIP_("Expression uses too much stack space"));
        return false;
      }
    }

    // push any remaining operators to output
    while (!operator_stack.is_empty()) {
      Token top = operator_stack.last();
      if (!output_op_or_function(top, output, stack_type, stack_size)) {
        error_msg = unsupported_type_error(top);
        return false;
      }
      operator_stack.discard_last();
    }

    // Push additional type conversion operations if necessary to make
    // sure the value on top of the stack is correct for the output type
    Token::eValueType top_type = stack_type.last();
    if (top_type == eValueType::INT && output_type_ != eNodeSocketDatatype::SOCK_BOOLEAN &&
        output_type_ != eNodeSocketDatatype::SOCK_INT)
    {
      // Unless we need an int type, convert to float so that code below
      // knows the top type is float or vec
      output.add_token(TokenType::CONVERT_INT_FLOAT, 0);
    }
    if (top_type == eValueType::VEC && output_type_ != eNodeSocketDatatype::SOCK_VECTOR) {
      // Need to convert a vector to a scalar type, so just take x
      output.add_token(TokenType::OPERATOR_GET_MEMBER_VEC, 2);
    }
    if (output_type_ == eNodeSocketDatatype::SOCK_VECTOR && top_type != eValueType::VEC) {
      // Just add two values to make the stack contain the vector(stack_top, 0, 0)
      output.add_token(TokenType::CONSTANT_FLOAT, 0);
      output.add_token(TokenType::CONSTANT_FLOAT, 0);
    }
    if (top_type != eValueType::INT && (output_type_ == eNodeSocketDatatype::SOCK_BOOLEAN ||
                                        output_type_ == eNodeSocketDatatype::SOCK_INT))
    {
      output.add_token(TokenType::CONVERT_FLOAT_INT, 0);
    }

    return true;
  }

  std::string unsupported_type_error(Token t)
  {
    return std::string(token_info[(int)t.type].name) + std::string(TIP_(": wrong data type."));
  }

  using output_variant = std::variant<float, int, bool, float3>;

  output_variant execute_program(Vector<GVArray> &inputs, int index) const
  {
    if (!program_valid_)
      return 0;

    const TokenQueue &program = program_buffer_;
    float stack[MAX_STACK];
    int top_idx = -1;  // index of top item on stack

    int num_ops = program.element_count();
    for (int i = 0; i < num_ops; i++) {
      Token t = program.at(i);
      switch (t.type) {
        case Token::TokenType::CONSTANT_FLOAT:
          push_float(stack, top_idx, t.get_value_as_float());
          break;
        case Token::TokenType::CONSTANT_INT:
          push_int(stack, top_idx, t.value);
          break;
        case Token::TokenType::VARIABLE_FLOAT: {
          float v = inputs[t.value].get<float>(index);
          push_float(stack, top_idx, v);
        } break;
        case Token::TokenType::VARIABLE_INT: {
          float v = inputs[t.value].get<int>(index);
          push_int(stack, top_idx, v);
        } break;
        case Token::TokenType::VARIABLE_BOOL: {
          bool b = inputs[t.value].get<bool>(index);
          int int_val = b ? 1 : 0;
          push_int(stack, top_idx, int_val);
        } break;
        case Token::TokenType::VARIABLE_VEC: {
          float3 vv = inputs[t.value].get<float3>(index);
          push_vector(stack, top_idx, vv);
        } break;
        case Token::TokenType::OPERATOR_PLUS: {
          auto [arg1, arg2] = pop_two_floats(stack, top_idx);
          float res = arg1 + arg2;
          push_float(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_PLUS_INT: {
          auto [arg1, arg2] = pop_two_ints(stack, top_idx);
          int res = arg1 + arg2;
          push_int(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_PLUS_VEC: {
          auto [arg1, arg2] = pop_two_vectors(stack, top_idx);
          float3 res = arg1 + arg2;
          push_vector(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_MINUS: {
          auto [arg1, arg2] = pop_two_floats(stack, top_idx);
          float res = arg1 - arg2;
          push_float(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_MINUS_INT: {
          auto [arg1, arg2] = pop_two_ints(stack, top_idx);
          int res = arg1 - arg2;
          push_int(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_MINUS_VEC: {
          auto [arg1, arg2] = pop_two_vectors(stack, top_idx);
          float3 res = arg1 - arg2;
          push_vector(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_MULTIPLY: {
          auto [arg1, arg2] = pop_two_floats(stack, top_idx);
          float res = arg1 * arg2;
          push_float(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_MULTIPLY_INT: {
          auto [arg1, arg2] = pop_two_ints(stack, top_idx);
          int res = arg1 * arg2;
          push_int(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_MULTIPLY_FLOAT_VEC: {
          float3 arg2 = pop_vector(stack, top_idx);
          float arg1 = pop_float(stack, top_idx);
          float3 res = arg1 * arg2;
          push_vector(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_MULTIPLY_VEC_FLOAT: {
          float arg2 = pop_float(stack, top_idx);
          float3 arg1 = pop_vector(stack, top_idx);
          float3 res = arg1 * arg2;
          push_vector(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_DIVIDE: {
          auto [arg1, arg2] = pop_two_floats(stack, top_idx);
          float res = arg1 / arg2;
          push_float(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_DIVIDE_INT: {
          auto [arg1, arg2] = pop_two_ints(stack, top_idx);
          int res = arg1 / arg2;
          push_int(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_DIVIDE_VEC_FLOAT: {
          float arg2 = pop_float(stack, top_idx);
          float3 arg1 = pop_vector(stack, top_idx);
          float3 res = arg1 / arg2;
          push_vector(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_POWER: {
          auto [arg1, arg2] = pop_two_floats(stack, top_idx);
          float res = pow(arg1, arg2);
          push_float(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_POWER_INT: {
          auto [arg1, arg2] = pop_two_ints(stack, top_idx);
          int res = pow(arg1, arg2);
          push_int(stack, top_idx, res);
        } break;
        case Token::TokenType::OPERATOR_UNARY_MINUS: {
          stack[top_idx] = -stack[top_idx];
        } break;
        case Token::TokenType::OPERATOR_UNARY_MINUS_INT: {
          int *int_stack = reinterpret_cast<int *>(stack);
          int_stack[top_idx] = -int_stack[top_idx];
        } break;
        case Token::TokenType::OPERATOR_UNARY_MINUS_VEC: {
          stack[top_idx] = -stack[top_idx];
          stack[top_idx - 1] = -stack[top_idx - 1];
          stack[top_idx - 2] = -stack[top_idx - 2];
        } break;
        case Token::TokenType::OPERATOR_GET_MEMBER_VEC: {
          int offset = t.value;
          float f = stack[top_idx - offset];
          top_idx -= 3;  // discard the vector
          push_float(stack, top_idx, f);
        } break;
        case Token::TokenType::FUNCTION_COSINE: {
          float res = cos(pop_float(stack, top_idx));
          push_float(stack, top_idx, res);
        } break;
        case Token::TokenType::FUNCTION_SINE: {
          float res = sin(pop_float(stack, top_idx));
          push_float(stack, top_idx, res);
        } break;
        case Token::TokenType::FUNCTION_SQUARE_ROOT: {
          float res = sqrt(pop_float(stack, top_idx));
          push_float(stack, top_idx, res);
        } break;
        case Token::TokenType::FUNCTION_MAX: {
          auto [a, b] = pop_two_floats(stack, top_idx);
          float res = a > b ? a : b;
          push_float(stack, top_idx, res);
        } break;
        case Token::TokenType::FUNCTION_MAX_INT: {
          auto [a, b] = pop_two_ints(stack, top_idx);
          int res = a > b ? a : b;
          push_int(stack, top_idx, res);
        } break;
        case Token::TokenType::CONVERT_INT_FLOAT: {
          int offset = t.value;  // may not be top of stack to convert
          int i = *(reinterpret_cast<int *>(&stack[top_idx - offset]));
          float res = (float)i;
          stack[top_idx - offset] = res;
        } break;
        case Token::TokenType::CONVERT_FLOAT_INT: {
          int offset = t.value;  // may not be top of stack to convert
          float f = stack[top_idx - offset];
          int res = (int)f;
          *(reinterpret_cast<int *>(&stack[top_idx - offset])) = res;
        } break;
        case Token::TokenType::LEFT_PAREN:
        case Token::TokenType::RIGHT_PAREN:
        case Token::TokenType::NONE:
          // These should not appear in executing programs
          break;
        default:
          BLI_assert_unreachable();
          break;
      }
    }

    // Get correct type off of stack and return it
    if (output_type_ == eNodeSocketDatatype::SOCK_FLOAT)
      return output_variant(pop_float(stack, top_idx));
    else if (output_type_ == eNodeSocketDatatype::SOCK_INT)
      return output_variant(pop_int(stack, top_idx));
    else if (output_type_ == eNodeSocketDatatype::SOCK_BOOLEAN) {
      int tos = pop_int(stack, top_idx);
      return output_variant(tos != 0);
    }
    else
      return output_variant(pop_vector(stack, top_idx));
  }

  // Utility methods to push a particular type onto the argument stack
  inline void push_float(float stack[], int &top_idx, float val) const
  {
    stack[++top_idx] = val;
  }

  inline void push_int(float stack[], int &top_idx, int val) const
  {
    *(reinterpret_cast<int *>(&(stack[++top_idx]))) = val;
  }

  inline void push_vector(float stack[], int &top_idx, float3 &val) const
  {
    stack[++top_idx] = val.x;
    stack[++top_idx] = val.y;
    stack[++top_idx] = val.z;
  }

  // Utility methods to pop a particular type from the argument stack
  inline float pop_float(float stack[], int &top_idx) const
  {
    return stack[top_idx--];
  }

  inline int pop_int(float stack[], int &top_idx) const
  {
    return *reinterpret_cast<int *>(&stack[top_idx--]);
  }

  inline float3 pop_vector(float stack[], int &top_idx) const
  {
    top_idx -= 3;
    return float3(stack[top_idx + 1], stack[top_idx + 2], stack[top_idx + 3]);
  }

  // Some utility methods to pop multiple args in one call, as this common
  struct PopTwoFloatsResults {
    float arg1;  // The argument pushed onto the stack first
    float arg2;
  };
  struct PopTwoIntsResults {
    int arg1;  // The argument pushed onto the stack first
    int arg2;
  };
  struct PopTwoVectorsResults {
    float3 arg1;  // The argument pushed onto the stack first
    float3 arg2;
  };

  inline PopTwoFloatsResults pop_two_floats(float stack[], int &top_idx) const
  {
    top_idx -= 2;
    return PopTwoFloatsResults{stack[top_idx + 1], stack[top_idx + 2]};
  }

  inline PopTwoIntsResults pop_two_ints(float stack[], int &top_idx) const
  {
    top_idx -= 2;
    int *int_stack = reinterpret_cast<int *>(stack);
    return PopTwoIntsResults{int_stack[top_idx + 1], int_stack[top_idx + 2]};
  }

  inline PopTwoVectorsResults pop_two_vectors(float stack[], int &top_idx) const
  {
    top_idx -= 6;
    return PopTwoVectorsResults{
        float3(stack[top_idx + 1], stack[top_idx + 2], stack[top_idx + 3]),
        float3(stack[top_idx + 4], stack[top_idx + 5], stack[top_idx + 6])};
  }
};

////////////////////////////////////////////////////////////////////////////
// ExpressionEvaluateFunction
// The multi-function used for evaluation
////////////////////////////////////////////////////////////////////////////
class ExpressionEvaluateFunction : public mf::MultiFunction {
  mf::Signature signature_;
  size_t first_output_idx_;  // Index of first output parameter
  Vector<std::string> input_identifiers_;
  Vector<short> input_types_;
  std::unique_ptr<ExpressionProgram> program_;

 public:
  ExpressionEvaluateFunction(const bNode &node, std::unique_ptr<ExpressionProgram> program)
      : program_(std::move(program))
  {
    const NodeGeometryExpression *node_storage = static_cast<NodeGeometryExpression *>(
        node.storage);

    CreateSignature(node);
    this->set_signature(&signature_);

    // Build a vector of input socket names and identifiers (excluding Expression input)
    for (int i = 1; i < node.input_sockets().size(); i++) {
      auto in_sock = node.input_sockets()[i];
      if (in_sock->typeinfo->base_cpp_type != nullptr) {
        input_identifiers_.append(in_sock->identifier);
        input_types_.append(in_sock->type);
      }
    }
  }

  // virtual ~ExpressionEvaluateFunction() {}

  void CreateSignature(const bNode &node)
  {
    mf::SignatureBuilder builder{"Expression", signature_};

    // Create the input parameters, skipping unconnected extend socket
    for (int i : node.input_sockets().index_range()) {
      if (i == 0)  // Skip Expression input
        continue;
      auto in_sock = node.input_sockets()[i];
      if (in_sock->typeinfo->base_cpp_type != nullptr)
        builder.single_input(in_sock->identifier, *in_sock->typeinfo->base_cpp_type);
    }

    // Create output params
    first_output_idx_ = signature_.params.size();
    builder.single_output("Result",
                          *bke::socket_type_to_geo_nodes_base_cpp_type(program_->output_type_));
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context context) const final
  {
    GMutableSpan results = params.uninitialized_single_output(first_output_idx_, "Result");

    // Gather the input arrays
    Vector<GVArray> input_arrays(input_identifiers_.size());
    for (int i = 0; i < input_identifiers_.size(); i++) {
      input_arrays[i] = params.readonly_single_input(i, input_identifiers_[i]);
    }

    if (program_->output_type_ == eNodeSocketDatatype::SOCK_FLOAT) {
      auto f_results = results.typed<float>();
      // Single value output type
// #define TEST
#ifdef TEST
      // Test output
      mask.foreach_index([&](const int64_t i) {
        float sum = 0;
        for (int n = 0; n < input_types_.size(); n++) {
          float val;
          switch (input_types_[n]) {
            case eNodeSocketDatatype::SOCK_FLOAT:
              val = input_arrays[n].get<float>(i);
              break;
            case eNodeSocketDatatype::SOCK_INT:
              val = input_arrays[n].get<int32_t>(i);
              break;
            case eNodeSocketDatatype::SOCK_BOOLEAN:
              val = input_arrays[n].get<bool>(i) ? 1.0f : 0.0f;
              break;
            case eNodeSocketDatatype::SOCK_VECTOR:
              val = input_arrays[n].get<float3>(i).x;
              break;
            default:
              // Unsupported type
              BLI_assert_unreachable();
              val = 0;
              break;
          }
          sum += val;
        }
        results[i] = sum;
      });
#else
      mask.foreach_index([&](const int64_t i) {
        auto val = program_->execute_program(input_arrays, i);
        f_results[i] = std::get<float>(val);
      });
#endif
    }
    else if (program_->output_type_ == eNodeSocketDatatype::SOCK_INT) {
      auto i_results = results.typed<int>();
      mask.foreach_index([&](const int64_t i) {
        auto val = program_->execute_program(input_arrays, i);
        i_results[i] = std::get<int>(val);
      });
    }
    else if (program_->output_type_ == eNodeSocketDatatype::SOCK_BOOLEAN) {
      auto i_results = results.typed<bool>();
      mask.foreach_index([&](const int64_t i) {
        auto val = program_->execute_program(input_arrays, i);
        i_results[i] = std::get<bool>(val);
      });
    }
    else if (program_->output_type_ == eNodeSocketDatatype::SOCK_VECTOR) {
      // Vector output type
      auto v_results = results.typed<float3>();
      mask.foreach_index([&](const int64_t i) {
        auto val = program_->execute_program(input_arrays, i);
        float3 res = std::get<float3>(val);
        v_results[i] = res;
      });
    }
  }

  ExecutionHints get_execution_hints() const final
  {
    ExecutionHints hints;
    hints.min_grain_size = 512;
    return hints;
  }
};

////////////////////////////////////////////////////////////////////////////
// Node Functions
// The standard set of funtions for the node
////////////////////////////////////////////////////////////////////////////

NODE_STORAGE_FUNCS(NodeGeometryExpression);

static bool is_supported_socket_type(const eNodeSocketDatatype data_type)
{
  return ELEM(data_type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_VECTOR);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  // Bizarrely these two lines are necessary to set b.is_context_dependent
  const bNodeTree *ntree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (node == nullptr)
    return;

  b.add_input<decl::String>("Expression").default_value(std::string("x + y"));

  // Add the variable number of input sockets
  const NodeGeometryExpression &storage = node_storage(*node);
  for (const NodeExpressionItem &eq_item : storage.socket_items.items()) {
    const std::string identifier = ExpressionItemsAccessor::socket_identifier_for_item(eq_item);
    const eNodeSocketDatatype dataType = (eNodeSocketDatatype)eq_item.socket_type;
    auto &input = b.add_input(dataType, eq_item.name, identifier)
                      .socket_name_ptr(
                          &ntree->id, ExpressionItemsAccessor::item_srna, &eq_item, "name");
    input.supports_field();
  }

  // Add extension socket
  b.add_input<decl::Extend>("", "__extend__");

  // Add outputs
  const eNodeSocketDatatype output_type = eNodeSocketDatatype(storage.output_type);
  auto output = b.add_output(output_type, "Result");
  output.dependent_field();
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryExpression *data = MEM_cnew<NodeGeometryExpression>(__func__);

  data->socket_items.next_identifier = 0;
  data->socket_items.items_array = nullptr;
  data->socket_items.items_num = 0;
  data->output_type = eNodeSocketDatatype::SOCK_FLOAT;

  node->storage = data;

  // Add a couple of predefined inputs
  data->socket_items.items_array = MEM_cnew_array<NodeExpressionItem>(2, __func__);
  ExpressionItemsAccessor::init_with_socket_type_and_name(
      *node, data->socket_items.items_array[0], eNodeSocketDatatype::SOCK_FLOAT, "x");
  ExpressionItemsAccessor::init_with_socket_type_and_name(
      *node, data->socket_items.items_array[1], eNodeSocketDatatype::SOCK_FLOAT, "y");
  data->socket_items.items_num = 2;
}

static void node_free_storage(bNode *node)
{
  NodeGeometryExpression *data = reinterpret_cast<NodeGeometryExpression *>(node->storage);
  if (!data)
    return;

  socket_items::destruct_array<ExpressionItemsAccessor>(*node);
  MEM_freeN(node->storage);
  node->storage = nullptr;  // free_storage seems to get called twice at shutdown, so protect
                            // against double free
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryExpression &src_storage = node_storage(*src_node);
  NodeGeometryExpression *dst_storage = (NodeGeometryExpression *)MEM_cnew<NodeGeometryExpression>(
      __func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<ExpressionItemsAccessor>(*src_node, *dst_node);
}

static bool node_insert_link(bNodeTree *ntree, bNode *node, bNodeLink *link)
{
  auto storage = reinterpret_cast<NodeGeometryExpression *>(node->storage);
  int starting_sockets_num = storage->socket_items.items_num;

  bool ok = socket_items::try_add_item_via_any_extend_socket<ExpressionItemsAccessor>(
      *ntree, *node, *node, *link);

  // If the link wasn't added or it's an output, we're done
  if (!ok || link->fromnode == node)
    return ok;

  // If we didn't add a new socket, then an existing one got reused. Check the type is valid as
  // try_add_item_via_any_extend_socket doesn't check this
  if (starting_sockets_num == storage->socket_items.items_num) {
    if (!ExpressionItemsAccessor::supports_socket_type(
            (eNodeSocketDatatype)(link->fromsock->type)))
      return false;
  }

  // Find the index of the added link
  // int item_index = storage->socket_items.items_num - 1;  // Newly added item is always last
  int item_index = -1;
  for (int i = 0; i < storage->socket_items.items_num; i++) {
    if (strcmp(link->tosock->name, storage->socket_items.items_array[i].name) == 0) {
      item_index = i;
      break;
    }
  }
  if (item_index == -1)
    return ok;  // shouldn't happen

  // Update the socket type
  storage->socket_items.items_array[item_index].socket_type = link->fromsock->type;

  // If we didn't add a new socket then no need to rename
  if (starting_sockets_num == storage->socket_items.items_num)
    return ok;

  // If we're connecting to a socket that's renamable, then keep the existing name
  if (link->fromnode->is_group_input() || link->fromnode->is_group_output())
    return ok;
  auto from_type = link->fromnode->idname;
  if (node->is_type("GeometryNodeRepeatInput") || node->is_type("GeometryNodeRepeatOutput") ||
      node->is_type("GeometryNodeForeachGeometryElementInput") ||
      node->is_type("GeometryNodeForeachGeometryElementOutput") ||
      node->is_group_input() /*node->is_type("NODE_GROUP")*/)
    return ok;

  // If the item has a single char name it's probably ok, so don't change it
  const char *item_name = storage->socket_items.items_array[item_index].name;
  if (strlen(item_name) == 1)
    return ok;

  // rename the new connect to something more convienient than the default
  const char *new_name = nullptr;
  bool free_new_name = false;
  if (item_index == 0)
    new_name = "x";
  else {
    auto prev_name = storage->socket_items.items_array[item_index - 1].name;
    new_name = ExpressionItemsAccessor::get_new_unique_name(*node, prev_name);
    free_new_name = true;
  }
  if (new_name) {
    MEM_SAFE_FREE(storage->socket_items.items_array[item_index].name);
    storage->socket_items.items_array[item_index].name = BLI_strdupn(new_name, strlen(new_name));
    if (free_new_name)
      MEM_SAFE_FREE(new_name);
  }

  return ok;
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "output_type", UI_ITEM_NONE, "", ICON_NONE);
  // uiItemR(layout, ptr, "axis", UI_ITEM_R_EXPAND, nullptr, ICON_NONE);
  // uiLayoutSetPropSep(layout, true);
  // uiLayoutSetPropDecorate(layout, false);
  // uiItemR(layout, ptr, "pivot_axis", UI_ITEM_NONE, IFACE_("Pivot"), ICON_NONE);
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *static_cast<bNode *>(ptr->data);

  uiItemR(layout, ptr, "output_type", UI_ITEM_NONE, "", ICON_NONE);

  if (uiLayout *panel = uiLayoutPanel(C, layout, "Expression_items", false, IFACE_("Variables"))) {
    socket_items::ui::draw_items_list_with_operators<ExpressionItemsAccessor>(
        C, panel, tree, node);
    socket_items::ui::draw_active_item_props<ExpressionItemsAccessor>(
        tree, node, [&](PointerRNA *item_ptr) {
          uiLayoutSetPropSep(panel, true);
          uiLayoutSetPropDecorate(panel, false);
          uiItemR(panel, item_ptr, "description", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  if (!params.output_is_required("Result"))
    return;

  // Get the Expression
  std::string Expression = params.get_input<std::string>("Expression");

  // If no Expression, do nothing
  if (Expression.size() == 0) {
    // params.error_message_add(NodeWarningType::Error, TIP_("Expression"));
    params.set_default_remaining_outputs();
    return;
  }

  // Get the output type
  const bNode &node = params.node();
  const NodeGeometryExpression &storage = *(const NodeGeometryExpression *)(node.storage);
  uint8_t oType = storage.output_type;

  // Build vectors of input names and types (excluding Expression socket, and extend socket)
  Vector<const char *> input_names;
  Vector<short> input_types;
  for (int i = 1; i < node.input_sockets().size(); i++) {
    auto in_sock = node.input_sockets()[i];
    if (in_sock->typeinfo->base_cpp_type != nullptr) {
      input_names.append(in_sock->name);
      input_types.append(in_sock->type);
    }
  }

  // Create a program from the expression
  std::string error_msg;
  std::unique_ptr<ExpressionProgram> program = std::make_unique<ExpressionProgram>(
      &input_names, &input_types, (eNodeSocketDatatype)oType);
  if (!program->create_program(Expression, error_msg)) {
    params.error_message_add(NodeWarningType::Error, error_msg);
    params.set_default_remaining_outputs();
    return;
  }

  // Build vectors of input fields, excluding initial name field
  // and final extend field
  Vector<GField> input_fields;
  for (int i = 1; i < node.input_sockets().size(); i++) {
    auto in_sock = node.input_sockets()[i];
    if (in_sock->typeinfo->base_cpp_type != nullptr) {
      GField f = params.extract_input<GField>(in_sock->identifier);
      input_fields.append(std::move(f));
    }
  }

  // Create a FieldOperation with a multi-function to do the actual evaluation
  auto mf = std::make_unique<ExpressionEvaluateFunction>(params.node(), std::move(program));
  GField f_calculated_results{FieldOperation::Create(std::move(mf), input_fields)};

  // And set the output to the FieldOperation
  params.set_output<GField>("Result", std::move(f_calculated_results));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "output_type",
      "Output Type",
      "",
      rna_enum_node_socket_data_type_items,
      NOD_storage_enum_accessors(output_type),
      SOCK_FLOAT,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(
            rna_enum_node_socket_data_type_items, [](const EnumPropertyItem &item) -> bool {
              return is_supported_socket_type(eNodeSocketDatatype(item.value));
            });
      });
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(params.other_socket().type);
  if (params.in_out() == SOCK_IN) {
    if (data_type == SOCK_STRING) {
      params.add_item(IFACE_("Expression"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeExpression");
        params.update_and_connect_available_socket(node, "Expression");
      });
    }
  }
  else {
    if (is_supported_socket_type(data_type)) {
      params.add_item(IFACE_("Results"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeExpression");
        node_storage(node).output_type = params.socket.type;
        params.update_and_connect_available_socket(node, "Result");
      });
    }
  }
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ExpressionItemsAccessor>();
}

static void node_register()
{
#ifndef NDEBUG
  token_info_check();
#endif

  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeExpression", std::nullopt);
  ntype.ui_name = "Expression";
  ntype.ui_description = "Evaluate a string as a mathmatical Expression";
  ntype.nclass = NODE_CLASS_CONVERTER;

  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.insert_link = node_insert_link;
  ntype.gather_link_search_ops = node_gather_link_searches;
  ntype.register_operators = node_operators;

  // blender::bke::node_register_type(&ntype);
  blender::bke::node_type_storage(
      &ntype, "NodeGeometryExpression", node_free_storage, node_copy_storage);

  // stash this auto assigmed value
  ExpressionItemsAccessor::node_type = ntype.type_legacy;

  blender::bke::node_register_type(&ntype);
  node_rna(ntype.rna_ext.srna);

  //  node_geo_Expression_cc::node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_expression_cc

////////////////////////////////////////////////////
// blender::nodes namespace
////////////////////////////////////////////////////
namespace blender::nodes {

StructRNA *ExpressionItemsAccessor::item_srna = &RNA_NodeExpressionItem;
int ExpressionItemsAccessor::node_type = 0;  // needs to be set during node registration
int ExpressionItemsAccessor::item_dna_type = SDNA_TYPE_FROM_STRUCT(NodeExpressionItem);

void ExpressionItemsAccessor::blend_write_item(BlendWriter *writer, const NodeExpressionItem &item)
{
  BLO_write_string(writer, item.name);
  BLO_write_string(writer, item.description);
}

void ExpressionItemsAccessor::blend_read_data_item(BlendDataReader *reader,
                                                   NodeExpressionItem &item)
{
  BLO_read_string(reader, &item.name);
  BLO_read_string(reader, &item.description);
}

}  // namespace blender::nodes

blender::Span<NodeExpressionItem> NodeExpressionItems::items() const
{
  return blender::Span<NodeExpressionItem>(items_array, items_num);
}

blender::MutableSpan<NodeExpressionItem> NodeExpressionItems::items()
{
  return blender::MutableSpan<NodeExpressionItem>(items_array, items_num);
}
