/*
 * Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.
 *
 * This code is licensed under the terms of the GPLv2
 * <http://www.gnu.org/licenses/old-licenses/gpl-2.0.html>, like most
 * MySQL Connectors. There are special exceptions to the terms and
 * conditions of the GPLv2 as it is applied to this software, see the
 * FLOSS License Exception
 * <http://www.mysql.com/about/legal/licensing/foss-exception.html>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
 */


#include "expr_parser.h"

PUSH_SYS_WARNINGS
#include <stdlib.h>
POP_SYS_WARNINGS


// TODO: Complete the parser


using namespace parser;
using cdk::string;
using cdk::Expression;
typedef cdk::Expression::Processor Processor;
typedef Processor::List_prc List_prc;

using cdk::Safe_prc;
using cdk::safe_prc;


/*
  Set up keyword and operator maps.
*/

Keyword::map_t  Keyword::kw_map;
Keyword         Keyword::init;

Op::tok_map_t   Op::unary_tok_map, Op::binary_tok_map;
Op::kw_map_t    Op::unary_kw_map, Op::binary_kw_map;
Op              Op::init;


// -------------------------------------------------------------------------


/*
  Variant of std::auto_ptr such that after smart_ptr.release() the
  pointed object can still be accessed via smart_ptr->xxx() (even
  though it is no longer owned by this smart_ptr instance).
*/

template <typename T>
class smart_ptr
  : public cdk::foundation::nocopy
{
  T *m_ptr;
  bool m_owns;

public:

  smart_ptr(T *ptr = NULL)
    : m_ptr(ptr), m_owns(true)
  {}

  ~smart_ptr()
  {
    reset(NULL);
  }

  void operator=(T *ptr)
  {
    reset(ptr);
  }

  T* reset(T *ptr)
  {
    if (m_owns)
      delete m_ptr;
    m_ptr = ptr;
    m_owns = true;
    return ptr;
  }

  T* release()
  {
    m_owns = false;
    return m_ptr;
  }

  T* operator->()
  {
    return m_ptr;
  }
};


/*
  Sink expression processor that ignores the expression reproted
  to it.

  It is used below in situations where we want to ignore results
  of parsing without storing them anywhere.
*/

struct Sink : public Expression::Processor
{
  Scalar_prc* scalar() { return NULL; }
  List_prc*   arr()    { return NULL; }
  Doc_prc*    doc()    { return NULL; }

};

Expression::Processor* ignore_if(Expression::Processor *prc)
{
  static Sink sink;
  if (!prc)
    return &sink;
  return prc;
}


bool Expr_parser_base::do_parse(Processor *prc)
{
  /*
    if prc is NULL, ignore the parsed expression instead of storing it
    which would be the case if we pass NULL to parse().
    For safety, delete the object returned from parse() if any.
  */
  delete parse(FULL, ignore_if(prc));

  return true;
}


// -------------------------------------------------------------------------


/**
   castOp ::= CAST LPAREN expr AS castType RPAREN
 */

bool Expr_parser_base::parse_cast(Scalar_prc *prc)
{
  if (!consume_token(Op::CAST))
    return false;

  Safe_prc<List_prc> ap = safe_prc(prc)->op(Op::name(Op::CAST));

  consume_token_throw(Token::LPAREN, L"Expected '(' after CAST");

  ap->list_begin();

  // 1st arg, the expression

  delete parse(FULL, ignore_if(ap->list_el()));

  consume_token_throw(Keyword::AS,
    L"Expected AS after expression inside CAST operator");

  // 2nd arg, cast_data_type

  ap->list_el()->scalar()->val()->value(cdk::TYPE_BYTES,
                                        Format_info(),
                                        cdk::bytes(parse_cast_type()));

  ap->list_end();

  consume_token_throw(Token::RPAREN,
    L"Expected ')' closing CAST operator call");

  return true;
}



/**
   castType ::=
        SIGNED INTEGER?
      |   UNSIGNED INTEGER?
      |   CHAR lengthSpec?
      |   BINARY lengthSpec?
      |   DECIMAL (lengthSpec | (LPAREN INT COMMA INT RPAREN))?
      |   TIME
      |   DATE
      |   DATETIME
      |   JSON

   lengthSpec ::= LPAREN INT RPAREN
 */

std::string Expr_parser_base::parse_cast_type()
{
  std::string type_str;

  const Token* token = consume_token();

  if (!token)
    parse_error(L"Expected cast type");

  Keyword::Type type = Keyword::get(*token);
  type_str = Keyword::name(type);

  switch (type)
  {

  case Keyword::BINARY:
  case Keyword::CHAR:
  case Keyword::DECIMAL:
    if (cur_token_type_is(Token::LPAREN))
      type_str += cast_data_type_dimension(Keyword::DECIMAL == type);
    break;

  case Keyword::SIGNED:
  case Keyword::UNSIGNED:
    consume_token(Keyword::Set{ Keyword::INTEGER, Keyword::INT });
    type_str += " ";
    type_str += Keyword::name(Keyword::INTEGER);
    break;

  case Keyword::DATE:
  case Keyword::DATETIME:
  case Keyword::TIME:
  case Keyword::INTEGER:
  case Keyword::JSON:
    break;

  default:
    parse_error(L"Expected cast type");

  }

  return type_str;
}


/**
   dimension ::= LPAREN LINTEGER RPAREN

   if double_dimention = true:
   LPAREN INT COMMA INT RPAREN

   returns textual representation of the parse, like "(N)" or "(N:M)".
 */

std::string Expr_parser_base::cast_data_type_dimension(bool double_dimension)
{
  consume_token_throw(Token::LPAREN, L"Expected type dimension specification");
  std::string result("(");
  result += consume_token_throw(
              Token::INTEGER,
              L"Expected integer type dimension"
            ).get_text();
  if (double_dimension && consume_token(Token::COMMA))
  {
    result += ",";
    result += consume_token_throw(
                Token::INTEGER,
                L"Expected second type dimension after ','"
              ).get_text();
  }
  result += ")";
  consume_token_throw(
    Token::RPAREN,
    L"Expected ')' closing type dimension specification"
  );
  return result;
}


// -------------------------------------------------------------------------

/*
   ident ::=
     ID
   | QUOTED_ID
 */

bool Expr_parser_base::get_ident(string &id)
{
  if (!tokens_available())
    return false;

  if (Token_base::cur_token_type_in({ Token::WORD, Token::QWORD }))
  {
    id = consume_token()->get_text();
    return true;
  }

  return false;
}


/*
  Assuming that a schema-qualified identifier was just parsed, attempt to
  parse a function call if next token starts argument list.
  Returns false if this is not the case.

  functionCall ::= schemaQualifiedIdent LPAREN argsList? RPAREN
*/

bool
Expr_parser_base::parse_function_call(const cdk::api::Table_ref &func, Scalar_prc *prc)
{
  if (!consume_token(Token::LPAREN))
    return false;

  List_prc *aprc = NULL;
  bool     qualified = (NULL != func.schema());
  bool     parse_position = false;

  // Report position(.. IN ..) as locate(...,...)

  if (! qualified && Keyword::equal(func.name(), L"position"))
  {
    Table_ref locate;
    locate.set(L"locate");
    aprc = safe_prc(prc)->call(locate);
    parse_position = true;
  }
  else
    aprc = safe_prc(prc)->call(func);

  if (aprc)
    aprc->list_begin();

  if (!cur_token_type_is(Token::RPAREN))
  {
    if (
      !qualified && Keyword::equal(func.name(), L"trim")
      && cur_token_type_in({
           Keyword::BOTH, Keyword::LEADING, Keyword::TRAILING
         })
    )
      unsupported(L"LEADING, TRAILING or BOTH clause inside function TRIM()");

    parse(parse_position ? COMP : FULL, aprc ? aprc->list_el() : NULL);

    if (consume_token(Token::COMMA))
      parse_argslist(aprc);
    else
      parse_special_args(func, aprc);
  }

  if (aprc)
    aprc->list_end();

  consume_token_throw(
    Token::RPAREN,
    L"Expected ')' to close function argument list"
  );

  return true;
}


void
Expr_parser_base::parse_special_args(
  const cdk::api::Table_ref &func,
  Expression::List::Processor *aprc
)
{
  if (func.schema())
    return;

  if (Keyword::equal(func.name(), L"char"))
  {
    if (cur_token_type_is(Keyword::USING))
      unsupported(L"USING clause inside function CHAR()");
    return;
  }

  if (Keyword::equal(func.name(), L"trim"))
  {
    if (cur_token_type_is(Keyword::FROM))
      unsupported(L"FROM clause inside function TRIM()");
  }

  if (Keyword::equal(func.name(), L"position"))
  {
    if (!consume_token(Keyword::IN))
      parse_error("Expected IN inside POSITION(... IN ...)");
    parse(FULL, aprc ? aprc->list_el() : NULL);
    return;
  }
}



/*
  Original grammar:

  // [[schema.]table.]ident
  columnIdent ::= (ident '.' (ident '.')?)? ident
        ('->' (('$' documentPath) | ("'$" documentPath "'")) )?

  is rewritten as:

  columnIdent ::= schemaQualifiedIdent columnIdent1
  columnIdent1 ::= ('.' ident)? ('->' ( columnIdentDocPath
                                | "'" columnIdentDocPath "'" ))?

  columnIdentDocPath ::= documentField // but require DOLLAR prefix
*/

/*
  Parse a schema-qualified identifier and store it as table/schema
  name of m_col_ref member. Schema name is optional.

  If types is not NULL then types of the consumed tokens are stored in this
  array.
*/

bool Expr_parser_base::parse_schema_ident(Token::Type (*types)[2])
{
  if (types)
  {
    (*types)[0] = Token::Type(0);
    (*types)[1] = Token::Type(0);
  }

  if (!tokens_available())
    return false;

  if (types)
    (*types)[0] = peek_token()->get_type();

  string name;

  if (!get_ident(name))
    return false;

  m_col_ref.m_table_ref.set(name);

  if (consume_token(Token::DOT))
  {
    if (!tokens_available())
      return false;

    if (types)
      (*types)[1] = peek_token()->get_type();

    string name1;

    if (!get_ident(name1))
      return false;

    m_col_ref.m_table_ref.set(name1, name);
  }

  return true;
}


void Expr_parser_base::parse_column_ident(Path_prc *prc)
{
  parse_schema_ident();
  parse_column_ident1(prc);
}


void Expr_parser_base::parse_column_ident1(Path_prc *prc)
{
  if (consume_token(Token::DOT))
  {
    string name;

    if (!get_ident(name))
      parse_error(L"Expected identifier after '.'");
    // Note: the table part was initialized in parse_schema_ident()
    m_col_ref.set_name(name);
  }
  else
  {
    // Re-interpret table name parsed by parse_schema_ident() as a
    // column name of the form [<table>.]<column>

    if (m_col_ref.table()->schema())
      m_col_ref.set(m_col_ref.table()->name(), m_col_ref.table()->schema()->name());
    else
      m_col_ref.set(m_col_ref.table()->name());
  }

  if (consume_token(Token::ARROW))
  {

    if (Token_base::cur_token_type_in({ Token::QSTRING, Token::QQSTRING }))
    {
      Tokenizer toks(consume_token()->get_text());
      It first = toks.begin();
      It last  = toks.end();
      Expr_parser_base path_parser(first, last, m_parser_mode);
      // TODO: Translate parse errors
      path_parser.parse_document_field(prc, true);
      if (first != last)
        parse_error(L"Unexpected characters in a quotted path component");
    }
    else
    {
      parse_document_field(prc, true);
    }
  }

}


// -------------------------------------------------------------------------


/**
  The original grammar was:

   documentField ::= ID documentPath? | DOLLAR documentPath

  Which makes "*", "**.foo" or "*.foo" not valid field specifications
  while "$[3]" is a valid specification.

  We modify the grammar so that "$[..]" is not valid while "*.." or "**.."
  are valid:

    documentField ::=
      | DOLLAR documentPathLeadingDot
      | documentPath

  The grammar of documentPath was adjusted so that the first
  path item can not be an array item ("[n]" or "[*]") and we can request
  a leading DOT before member items (see parse_document_path()).

   If prefix is true, only the first form starting with DOLLAR prefix is
   accepted.
*/

void Expr_parser_base::parse_document_field(Path_prc *prc, bool prefix)
{
  if (consume_token(Token::DOLLAR))
  {
    if (!parse_document_path(prc, true))  // require DOT before members
      parse_error(L"Expected '.' to start a path after '$'");
    return;
  }

  if (prefix)
    parse_error(L"Expected '$' to start a document path");

  if (!parse_document_path(prc, false))
    parse_error(L"Expected a document path");
}


/*
  Parse a document field path with a given initial member segment.
*/

void Expr_parser_base::parse_document_field(const string &first, Path_prc *prc)
{
  Safe_prc<Path_prc> sprc = prc;

  sprc->list_begin();
  sprc->list_el()->member(first);
  parse_document_path1(prc);
  sprc->list_end();
}

/*
  Parse a document field path with given 2 initial member segment.
*/

void Expr_parser_base::parse_document_field(const string &first,
                                            const string &second,
                                            Path_prc *prc)
{
  Safe_prc<Path_prc> sprc = prc;

  sprc->list_begin();
  sprc->list_el()->member(first);
  sprc->list_el()->member(second);
  parse_document_path1(prc);
  sprc->list_end();
}

/**
  Original Grammar:

   documentPath ::= documentPathItem* documentPathLastItem

   documentPathItem ::=
          documentPathLastItem
      |   DOUBLESTAR

   documentPathLastItem ::=
          ARRAYSTAR
      |   LSQBRACKET INT RSQBRACKET
      |   DOTSTAR
      |   DOT documentPathMember

   documentPathMember ::=
        ID
      | STRING1

  This grammar has few flaws:

  1. It allows a document path to start with array location, which is not
     correct - array locations should be possible only after a path to some
     array member.

  2. It always requires a DOT befre a member element, but in some contexts
     we want a document path like "foo.bar.baz" to start without a dot.

  To deal with this the grammar has been changed and require_dot parameter
  has been added. Modified grammar:

   documentPath ::= documentPathFirstItem documentPathItem*

   documentPathFirstItem ::=
    | DOT? documentPathMember
    | DOUBLESTAR

   documentPathItem ::=
    | DOT documentPathMember
    | DOUBLESTAR
    | documentPathArray

   documentPathMember ::=
    | MUL
    | ID
    | STRING1

   docuemntPathArray ::= LSQBRACKET documentPathArrayLoc RSQBRACKET

   documentPathArrayLoc ::=
    | MUL
    | INT

  Parameter require_dot tells if the initial dot is required or not.

  A check that DOUBLESTAR is not last element of a path is done separately.

  Returns true if a valid document path was parsed and reported, false if the
  current token did not start a valid document path.

  Note: If false is returned then nothing is reported to the processor (not
  even an empty list).
*/

bool Expr_parser_base::parse_document_path(Path_prc *prc, bool require_dot)
{
  /*
    Below we call methods like parse_docpath_member() which expect a document
    path element processor. Our path processor prc is a list processor. So,
    before we report the first path element we must call prc->list_begin() and
    prc->list_el(). The problem is that when calling parse_docpath_member()
    we might not know yet if there is any path to report or not -- only inside
    parse_docpath_member() it will become evident.

    The Path_el_reporter wrapper around path processor solves this problem by
    deffering the initial list_begin() call and the list_el() calls to the
    moment when a path element is reported. If no path elements are reported
    then list_begin() or list_el() will not be called. Similar, call to
    list_end() will be forwarded to the wrapped processor only if list_begin()
    was called before.
  */

  struct Path_el_reporter
    : public Path_prc
    , public Path_prc::Element_prc
  {
    Safe_prc<Path_prc> m_prc;
    bool m_started;

    void list_begin()
    {
      if (!m_started)
        m_prc->list_begin();
      m_started = true;
    }

    void list_end()
    {
      if (m_started)
        m_prc->list_end();
    }

    Element_prc* list_el()
    {
      return this;
    }

    // Element_prc

    void member(const string &name)
    {
      list_begin();
      m_prc->list_el()->member(name);
    }

    void any_member()
    {
      list_begin();
      m_prc->list_el()->any_member();
    }

    void index(index_t ind)
    {
      list_begin();
      m_prc->list_el()->index(ind);
    }

    void any_index()
    {
      list_begin();
      m_prc->list_el()->any_index();
    }

    void any_path()
    {
      list_begin();
      m_prc->list_el()->any_path();
    }

    Path_el_reporter(Path_prc *prc)
      : m_prc(prc), m_started(false)
    {}
  }
  el_reporter(prc);

  // documentPathFirstItem

  bool double_star = false;

  if (consume_token(Token::DOUBLESTAR))
  {
    double_star = true;
    el_reporter.any_path();
  }
  else if (parse_docpath_member_dot(&el_reporter))
  {
    // continue below
  }
  else if (require_dot)
  {
    return false;
  }
  else
  {
    if (!parse_docpath_member(&el_reporter))
      return false;
  }

  // the rest of the path (if any)

  bool ret = parse_document_path1(&el_reporter);

  if (!ret && double_star)
    parse_error(L"Document path ending in '**'");

  el_reporter.list_end();

  return true;
}


/*
  Parse a reminder of a document path after the first item, that is, a possibly
  empty sequence of documentPathItem strings.

  The items are reported to the given Path_prc without calling list_begin() or
  list_end() (which is assumed to be done by the caller).

  Returns true if at least one path item component was parsed.
*/

bool Expr_parser_base::parse_document_path1(Path_prc *prc)
{
  Safe_prc<Path_prc> sprc = prc;

  /*
    These Booleans are used to detect if we are at the beginning of the path
    and if there was a "**" component at the end of it.
  */

  bool double_star;
  bool last_double_star = false;
  bool has_item = false;

  for (double_star = false; true;
       last_double_star = double_star,
       double_star =false,
       has_item = true)
  {
    if (!cur_token_type_in({ Token::DOUBLESTAR, Token::DOT, Token::LSQBRACKET }))
      break;

    if (consume_token(Token::DOUBLESTAR))
    {
      sprc->list_el()->any_path();
      double_star = true;
      continue;
    }

    if (parse_docpath_member_dot(sprc))
      continue;

    if (parse_docpath_array(sprc))
      continue;

    break;
  }

  if (last_double_star)
    parse_error(L"Document path ending in '**'");

  return has_item;
}


/**
    documentPathMember ::=
      | MUL
      | ID
      | STRING1

    TODO: Does STRING1 differ from plain STRING in any way?
*/

bool Expr_parser_base::parse_docpath_member(Path_prc *prc)
{
  const Token *t = peek_token();

  if (!t)
    return false;

  switch (t->get_type())
  {
  case Token::STAR:
    safe_prc(prc)->list_el()->any_member();
    break;

  case Token::WORD:
  case Token::QQSTRING:
  case Token::QSTRING:
    safe_prc(prc)->list_el()->member(t->get_text());
    break;

  default:
    return false;
  }

  consume_token();
  return true;
}

bool Expr_parser_base::parse_docpath_member_dot(Path_prc *prc)
{
  if (!consume_token(Token::DOT))
    return false;
  if (!parse_docpath_member(prc))
    parse_error(L"Expected member name or '*' after '.' in a document path");
  return true;
}

/**
   docuemntPathArray ::= LSQBRACKET documentPathArrayLoc RSQBRACKET

   documentPathArrayLoc ::=
    | MUL
    | INT
 */

bool Expr_parser_base::parse_docpath_array(Path_prc *prc)
{
  if (!consume_token(Token::LSQBRACKET))
    return false;

  if (consume_token(Token::STAR))
  {
    safe_prc(prc)->list_el()->any_index();
  }
  else
  {
    if (!cur_token_type_is(Token::INTEGER))
      parse_error(L"Expected '*' or integer index after '[' in a document path");

    uint64_t v;

    try {
      v = strtoui(consume_token()->get_text());
    }
    catch (const Numeric_conversion_error &e)
    {
      parse_error(e.msg());
      throw;  // quiet compile warnings
    }

    if (v > std::numeric_limits<Path_prc::Element_prc::index_t>::max())
      parse_error(L"Array index too large");

    safe_prc(prc)->list_el()->index(Path_prc::Element_prc::index_t(v));
  }

  consume_token_throw(
    Token::RSQBRACKET,
    L"Expected ']' to close a document path array component"
  );
  return true;
}


// -------------------------------------------------------------------------


bool column_ref_from_path(cdk::Doc_path &path, parser::Column_ref &column)
{
  struct Path_prc
    : public cdk::Doc_path::Processor
    , public cdk::Doc_path::Processor::Element_prc
  {
    unsigned m_len;
    parser::Column_ref &m_col;
    bool m_ret;

    Element_prc* list_el()
    {
      return this;
    }

    void member(const string &name)
    {
      switch (m_len++)
      {
      case 0: m_col.set(name); break;
      case 1: m_col.set(name, m_col.name()); break;
      case 2:
        assert(m_col.table());
        m_col.m_table_ref.set(m_col.name(), m_col.table()->name());
        m_col.set_name(name);
        break;
      default:
        // Too many path elements
        m_ret = false;
      }
    }

    void index(uint32_t)
    {
      m_ret = false;
    }

    void any_member()
    {
      m_ret = false;
    }

    void any_index()
    {
      m_ret = false;
    }

    void any_path()
    {
      m_ret = false;
    }

    Path_prc(parser::Column_ref &col)
      : m_len(0), m_col(col), m_ret(true)
    {}
  }
  prc(column);

  path.process(prc);

  return prc.m_ret;
}


/**
   atomicExpr ::=
        placeholder
    |   columnIdent     // TABLE mode
    |   documentField   // DOCUMENT mode
    |   functionCall
    |   groupedExpr
    |   unaryOp
    |   castOp
    |   literal
    |   jsonDoc
    |   array

  placeholder ::= COLON ID

  groupedExpr ::= LPAREN expr RPAREN

  unaryOp ::=
          BANG atomicExpr
      |   NEG atomicExpr
      |   PLUS atomicExpr
      |   MINUS atomicExpr

  literal ::=
          INT
      |   FLOAT
      |   STRING1
      |   STRING2
      |   NULL
      |   FALSE
      |   TRUE

  We extend this grammar with nullary operators:

  nullaryOp ::= MUL

  TODO: "default" operator
 */

Expression* Expr_parser_base::parse_atomic(Processor *prc)
{
  if (!tokens_available())
    parse_error(L"Expected an expression");

  Token::Type type = peek_token()->get_type();

  switch (type)
  {
    // jsonDOC

  case Token::LCURLY:
    return parse(DOC, prc);

    // array

  case Token::LSQBRACKET:
    return parse(ARR, prc);

    // groupedExpr

  case Token::LPAREN:
  {
    consume_token();
    Expression *res = parse(FULL, prc);
    consume_token_throw(
      Token::RPAREN,
      L"Expected ')' to close parenthesized sub-expression"
    );
    return res;
  }

  default: break;
  }

  /*
    If prc is NULL, we are supposed to store and return the result
    of parsing. In that case initialize stored variable with appropriate
    storage object and set prc to point at it so that expression will
    be reported to the storage object.

    Note: if prc is not NULL then stored remains empty and stored.release()
    would produce NULL as required in this case.
  */

  smart_ptr<Stored_expr> stored;

  if (!prc)
    prc = stored.reset(new Stored_any());

  Safe_prc<Processor::Scalar_prc> sprc(prc->scalar());

  // parameters, nullary operators, CAST

  if (consume_token(Token::COLON))
  {
    sprc->param(consume_token_throw(
      Token::WORD,
      L"Expected parameter name after ':'"
    ).get_text());
    return stored.release();
  }

  if (consume_token(Op::STAR))
  {
    sprc->op(Op::name(Op::STAR));
    // NOTE: arguments processor is ignored as there are no arguments
    return stored.release();
  }

  if (parse_cast(sprc))
  {
    return stored.release();
  }

  // Unary operator.

  List_prc *argsp = NULL;
  bool     neg = false;

  Op::Type  op = Op::get_unary(*peek_token());

  switch (op)
  {
  case Op::PLUS:
  case Op::MINUS:
  {
    consume_token();
    if (Token_base::cur_token_type_in({ Token::NUMBER, Token::INTEGER }))
    {
      // treat as numeric literal with possibly negated value
      neg = (Op::MINUS == op);
      break;
    }
    // otherwise report as unary operator
    argsp = sprc->op(Op::name(op));
    break;
  }

  case Op::NEG:
    consume_token();
    argsp = sprc->op(Op::name(Op::NEG));
    break;
  case Op::NOT:
    consume_token();
    argsp = sprc->op(Op::name(Op::NOT));
    break;
  case Op::BITNEG:
    consume_token();
    argsp = sprc->op(Op::name(Op::BITNEG));
    break;

  default:
    break;  // will continue with literal parsing
  }

  // Report the single argument of the unary operator

  if (argsp)
  {
    argsp->list_begin();
    parse(ATOMIC, argsp->list_el());
    argsp->list_end();
    return stored.release();
  }

  assert(tokens_available());


  // Literal value

  Keyword::Type kw = Keyword::get(*peek_token());

  switch (kw)
  {

  case Keyword::L_NULL:
    sprc->val()->null();
    consume_token();
    return stored.release();

  case Keyword::L_TRUE:
  case Keyword::L_FALSE:
    sprc->val()->yesno(Keyword::L_TRUE == kw);
    consume_token();
    return stored.release();

  default:
    // continue looking for other literals
    break;
  }


  try {

    switch (peek_token()->get_type())
    {
    case Token::QQSTRING:
    case Token::QSTRING:
      if (m_strings_as_blobs)
      {
        // NOTE: if string is reported as blob, it gets converted to utf8
        sprc->val()->value(cdk::TYPE_BYTES, Format_info(),
          cdk::bytes(consume_token()->get_text()));
      }
      else
        sprc->val()->str(consume_token()->get_text());
      return stored.release();

    case Token::NUMBER:
      {
        double val = strtod(consume_token()->get_text());
        sprc->val()->num(neg ? -val : val);
        return stored.release();
      }

    case Token::INTEGER:
      if (neg)
      {
        int64_t val = strtoi(consume_token()->get_text());
        sprc->val()->num(-val);
      }
      else
      {
        uint64_t val = strtoui(consume_token()->get_text());
        sprc->val()->num(val);
      }
      return stored.release();

    case Token::HEX:
      if (neg)
      {
        int64_t val = strtoi(consume_token()->get_text(), 16);
        sprc->val()->num(-val);
      }
      else
      {
        uint64_t val = strtoui(consume_token()->get_text(), 16);
        sprc->val()->num(val);
      }
      return stored.release();

    default:
      // will continue with functionCall | columnIdent | documentField parsing
      break;
    }

  }
  catch (const Numeric_conversion_error &e)
  {
    parse_error(e.msg());
  }



  /*
    functionCall | columnIdent | documentField

    It is not possible to tell which of these 3 alternatives we have by
    looking at the current token. Either functionCall or columnIdent or
    documentField can start with something which looks like a schema-qualified
    name: "A" or "A.B".

    For that reason we start with a call to parse_schema_indent() which would
    parse such a schema-qualified name and store it as table/schema name of
    m_col_ref member.

    After this we try to parse a function call and if it fails we try
    columnIndent or documentField, depending on the parsing mode.
  */

  Token::Type types[2];
  bool schema_ident = false;

  m_col_ref.clear();

  /*
    Try to parse schema-qualified identifier, storing the types of the tokens
    that have been consumed - this information is needed in case parsing
    schema identifier fails.

    Note: it is important that parse_schema_ident() stores consumed tokens
    in m_col_ref even if it fails in the end.
  */

  schema_ident = parse_schema_ident(&types);

  /*
    If parse_schema_ident() succeeded, and we have the result in
    m_col_ref.table(), we see if it is not a beginning of a function call.
    If parse_function_call() succeeds then we are done.
  */

  if (schema_ident)
  {
    assert(m_col_ref.table());

    if (parse_function_call(*m_col_ref.table(), sprc))
      return stored.release();
  }

  /*
    Otherwise we must have either a document path (in DOCUMENT mode) or
    a column identifier, possibly followed by a path (in TABLE mode).
  */

  cdk::Doc_path_storage path;

  if (Parser_mode::TABLE == m_parser_mode)
  {
    /*
      If we are in the TABLE mode, and parse_schema_ident() failed above, then
      we do not have a valid column identifier which is an error.
    */

    if (!schema_ident)
      parse_error("Expected atomic expression");

    /*
      Otherwise we complete parsing the column identifier and report it to
      the processor.
    */

    parse_column_ident1(&path);
    sprc->ref(m_col_ref, path.is_empty() ? NULL : &path);
    return stored.release();
  }

  /*
    Here we know that we are in DOCUMENT mode and we are expecting a document
    path. If parse_schema_ident() called above consumed some tokens, we check
    if they were not quotted identifiers. Such identifiers are allowed when
    reffering to tables or columns but are invalid in a document path.
  */

  if (Token::QWORD == types[0] || Token::QWORD == types[1])
    parse_error("Expected atomic expression");

  /*
    Now we treat the identifiers "A.B" parsed by parse_schema_ident() and
    stored as table/schema name in m_col_ref (if any), as an initail segment
    of a document filed reference and complete parsing the whole document
    field.
  */

  if (m_col_ref.table() && m_col_ref.table()->schema())
  {
    parse_document_field(
      m_col_ref.table()->schema()->name(),
      m_col_ref.table()->name(),
      &path
    );
  }
  else if (m_col_ref.table())
  {
    parse_document_field(m_col_ref.table()->name(), &path);
  }
  else
  {
    parse_document_field(&path);
  }

  sprc->ref(path);

  return stored.release();
}


// -------------------------------------------------------------------------


Expression*
Expr_parser_base::left_assoc_binary_op(const Op::Set &ops,
                                       Start lhs, Start rhs,
                                       Processor *prc)
{
  /*
    An optimization under assumption that array or document can never be
    an argument of binary operator.

    If we see '{' or '[' then we do not expect the binary operator later
    (the whole expression consists of the LHS only). Thus, there is no need
    to store LHS for later reporting.
  */

  if (cur_token_type_is(Token::LCURLY))
    return parse(DOC, prc);

  if (cur_token_type_is(Token::LSQBRACKET))
    return parse(ARR, prc);


  // Store LHS of the expression

  smart_ptr<Expression> stored_lhs(parse(lhs, NULL));

  const Token *t = consume_token(ops);

  if (!t)
  {
    /*
      There is no RHS, so LHS is the whole expression.
      If prc is NULL then we return already stored LHS. Otherwise
      we report stored LHS to the processor.
    */

    if (!prc)
      return stored_lhs.release();

    stored_lhs->process(*prc);
    return NULL;
  }

  Op::Type op = Op::get_binary(*t);

  /*
    If storing operator call expression (prc is NULL), use specialized
    Stored_op class that can re-use already stored LHS expression.
  */

  smart_ptr<Stored_expr> stored;

  if (!prc)
    // Note: Stored_op takes ownership of the stored LHS expr.
    prc = stored.reset(new Stored_op(stored_lhs.release()));

  // pass lhs and rhs as operator arguments

  List_prc *aprc = safe_prc(prc)->scalar()->op(Op::name(op));

  if (aprc)
  {
    aprc->list_begin();

    // Report stored LHS as the 1st argument.

    stored_lhs->process_if(aprc->list_el());

    // then parse rhs, passing it as 2nd argument

    parse(rhs, aprc->list_el());

    aprc->list_end();
  }

  return stored.release();
}


Expression* Expr_parser_base::parse_mul(Processor *prc)
{
  Op::Set ops;
  ops.insert(Op::MUL);
  ops.insert(Op::DIV);
  ops.insert(Op::MOD);
  return left_assoc_binary_op(ops, ATOMIC, MUL, prc);
}


Expression* Expr_parser_base::parse_add(Processor *prc)
{
  Op::Set ops;
  ops.insert(Op::ADD);
  ops.insert(Op::SUB);
  return left_assoc_binary_op(ops, MUL, ADD, prc);
}

Expression* Expr_parser_base::parse_shift(Processor *prc)
{
  Op::Set ops;
  ops.insert(Op::LSHIFT);
  ops.insert(Op::RSHIFT);
  return left_assoc_binary_op(ops, ADD, SHIFT, prc);
}

Expression* Expr_parser_base::parse_bit(Processor *prc)
{
  if (consume_token(Op::BITNEG))
  {
    smart_ptr<Stored_expr> stored;

    if (!prc)
      prc = stored.reset(new Stored_any());

    Safe_prc<Processor::Scalar_prc> sprc(prc->scalar());
    List_prc *argsp = NULL;
    argsp = sprc->op(Op::name(Op::BITNEG));
    if (argsp)
    {
      argsp->list_begin();
      parse(ATOMIC, argsp->list_el());
      argsp->list_end();
      return stored.release();
    }

    return parse_bit(prc);
  }

  Op::Set ops;
  ops.insert(Op::BITAND);
  ops.insert(Op::BITOR);
  ops.insert(Op::BITXOR);
  return left_assoc_binary_op(ops, SHIFT, BIT, prc);
}

Expression* Expr_parser_base::parse_comp(Processor *prc)
{
  Op::Set ops;
  ops.insert(Op::GE);
  ops.insert(Op::GT);
  ops.insert(Op::LE);
  ops.insert(Op::LT);
  ops.insert(Op::EQ);
  ops.insert(Op::NE);
  return left_assoc_binary_op(ops, BIT, COMP, prc);
}

Expression* Expr_parser_base::parse_and(Processor *prc)
{
  return left_assoc_binary_op({ Op::AND }, ILRI, AND, prc);
}

Expression* Expr_parser_base::parse_or(Processor *prc)
{
  return left_assoc_binary_op({ Op::OR }, AND, OR, prc);
}


// -------------------------------------------------------------------------

/**
   Expression Parser EBNF:
   note; No reptetition, must be connected by logical operators
    ilriExpr ::=
            compExpr IS NOT? (NULL|TRUE|FALSE)
        |   compExpr NOT? IN LPAREN argsList? RPAREN
            // TODO: param to ESCAPE should be better defined
        |   compExpr NOT? LIKE compExpr (ESCAPE compExpr)?
        |   compExpr NOT? RLIKE compExpr (ESCAPE compExpr)?
        |   compExpr NOT? BETWEEN compExpr AND compExpr
        |   compExpr NOT? REGEXP compExpr
        |   compExpr
 */

Expression* Expr_parser_base::parse_ilri(Processor *prc)
{
  // Array/doc optimization like in left_assoc_binary_op().

  if (cur_token_type_is(Token::LCURLY))
    return parse(DOC, prc);

  if (cur_token_type_is(Token::LSQBRACKET))
    return parse(ARR, prc);


  // Store the first expression.

  smart_ptr<Expression> first(parse(COMP, NULL));

  // Record negation, if present.

  bool neg = (NULL != consume_token(Op::NOT));

  /*
    Look for the main operator.
  */

  Op::Set next;
  next.insert(Op::IS);
  next.insert(Op::IN);
  next.insert(Op::LIKE);
  next.insert(Op::RLIKE);
  next.insert(Op::BETWEEN);
  next.insert(Op::REGEXP);
  next.insert(Op::SOUNDS_LIKE);

  const Token *t = consume_token(next);

  /*
    If we don't see any of the operators and there was no negation
    then we report the first expression as complete ilriExpr.
  */

  if (!t)
  {
    if (neg)
      parse_error(L"Expected IN, (R)LIKE, BETWEEN or REGEXP after NOT");

    // If prc is NULL return already stored expression.

    if (!prc)
      return first.release();

    // Otherwise report stored expression to the processor.

    first->process(*prc);
    return NULL;
  }

  // Handle IS NOT case.

  if (neg && Op::IS == *t)
    parse_error(L"Operator NOT before IS, should be IS NOT");

  if (Op::IS == *t && consume_token(Op::NOT))
    neg = true;

  // We have an ilri expression with operator and 2 arguments.

  Op::Type op = Op::get_binary(*t);

  // Detect unsupported operators before handling parmaeters

  switch (op)
  {
  case Op::SOUNDS_LIKE:
    if (cur_token_type_is(Keyword::LIKE))
      unsupported(L"Operator SOUNDS LIKE");

  default: break;
  }

  /*
    If prc is NULL and we are supposed to store parsed epression, use
    specialized Stored_ilri class that can re-use the already stored first
    part of the expression.
  */

  List_prc *not_arg_prc = NULL;
  smart_ptr<Stored_ilri> stored;

  if (!prc)
  {
    // Note: Stored_ilri handles wrapping in "not" operator if needed.
    prc = stored.reset(new Stored_ilri(first.release(), neg));
  }
  else if (neg)
  {
    /*
      In case of negation, the main operator is wrapped in unary "not" operator.
      We report "not" operator with 1 argument and replace prc with the processor
      of this single argument. This way the code below will report the main
      expression as the argument of the "not" operator. Without negation prc
      will remain pointing at the original processor.
    */

    not_arg_prc = safe_prc(prc)->scalar()->op(Op::name(Op::NOT));
    if (not_arg_prc)
      not_arg_prc->list_begin();
    prc = safe_prc(not_arg_prc)->list_el();
  }

  // report the main operator

  Safe_prc<List_prc> aprc = safe_prc(prc)->scalar()->op(Op::name(op));
  aprc->list_begin();

  // 1st argument

  first->process_if(aprc->list_el());

  // other arguments

  switch (op)
  {
    case Op::IS:
    {
      t = consume_token();

      if (t)
      {
        switch (Keyword::get(*t))
        {
        case Keyword::L_TRUE:  aprc->list_el()->scalar()->val()->yesno(true); break;
        case Keyword::L_FALSE: aprc->list_el()->scalar()->val()->yesno(true); break;
        case Keyword::L_NULL: aprc->list_el()->scalar()->val()->null(); break;
        default:
          t = NULL; // this indicates error
        }
      }

      if (!t)
        parse_error("expected TRUE, FALSE or NULL after IS");

      break;
    }

    case Op::IN:
    {
      consume_token_throw(
        Token::LPAREN,
        L"Expected '(' after IN"
      );

      // Note: true flag means that strings will be reported as blobs.
      parse_argslist(aprc, true);

      consume_token_throw(
        Token::RPAREN,
        L"Expected ')' to close IN(... expression"
      );

      break;
    }

    case Op::LIKE:
    case Op::RLIKE:
    {
      parse(COMP, aprc->list_el());

      if (cur_token_type_is(Keyword::ESCAPE))
      {
        //TODO: Check ESCAPE
        unsupported("ESCAPE clause for (R)LIKE operator");
      }

      break;
    }

    case Op::REGEXP:
      parse(COMP, aprc->list_el());
      break;

    case Op::BETWEEN:
      parse(COMP, aprc->list_el());
      consume_token_throw(
        Keyword::AND,
        L"Expected AND in BETWEEN ... expression"
      );
      parse(COMP, aprc->list_el());
      break;

    default: assert(false);
  }

  // close argument list

  aprc->list_end();
  if (not_arg_prc)
    not_arg_prc->list_end();

  return stored.release();
}


// -------------------------------------------------------------------------

/*
  Below we want to use Expr_parser_base with parser templates such
  as Doc_parser<> or List_parser<>. These templates assume that the
  base parser can be constructed with a constructor which accepts only
  2 parameters defining the range of tokens to be parsed.

  But Expr_parser_base constructor also needs parser mode parameter
  and the flag which tells if strings should be reported as blobs.
  To fix this, we define Base_parser<> template parametrized with parser
  mode, which will construct required flavour of the parser.
*/

template <Parser_mode::value Mode,
          bool strings_as_blobs = false>
struct Base_parser : public Expr_parser_base
{
  Base_parser(It &first, const It &last)
    : Expr_parser_base(first, last, Mode, strings_as_blobs)
  {}
};


template <Parser_mode::value Mode,
          bool strings_as_blobs>
void parse_args(Processor::List_prc *prc, It &first, const It &last)
{
  List_parser< Base_parser<Mode, strings_as_blobs> >
    args_parser(first, last);
  args_parser.process_if(prc);
}

template <bool strings_as_blobs>
void parse_args(Parser_mode::value mode, Processor::List_prc *prc,
                It &first, const It &last)
{
  if (Parser_mode::DOCUMENT == mode)
    parse_args<Parser_mode::DOCUMENT, strings_as_blobs>(prc, first, last);
  else
    parse_args<Parser_mode::TABLE, strings_as_blobs>(prc, first, last);
}


void
Expr_parser_base::parse_argslist(Processor::List_prc *prc,
                                 bool strings_as_blobs)
{
  /*
     argsList ::= expr (COMMA expr)*
  */

  if (strings_as_blobs)
    parse_args<true>(m_parser_mode, prc, cur_pos(), end_pos());
  else
    parse_args<false>(m_parser_mode, prc, cur_pos(), end_pos());
}


void Expr_parser_base::parse_arr(Processor::List_prc *prc)
{
  if (Parser_mode::DOCUMENT == m_parser_mode)
  {
    Arr_parser<Base_parser<Parser_mode::DOCUMENT>,
               Expression::Scalar::Processor>
      arr_parser(cur_pos(), end_pos());
    arr_parser.process_if(prc);
  }
  else
  {
    Arr_parser<Base_parser<Parser_mode::TABLE>, Expression::Scalar::Processor>
      arr_parser(cur_pos(), end_pos());
    arr_parser.process_if(prc);
  }
}


void Expr_parser_base::parse_doc(Processor::Doc_prc *prc)
{
  if (Parser_mode::DOCUMENT == m_parser_mode)
  {
    Doc_parser<Base_parser<Parser_mode::DOCUMENT>,
               Expression::Scalar::Processor>
      doc_parser(cur_pos(), end_pos());
    doc_parser.process_if(prc);
  }
  else
  {
    Doc_parser<Base_parser<Parser_mode::TABLE>,
               Expression::Scalar::Processor>
      doc_parser(cur_pos(), end_pos());
    doc_parser.process_if(prc);
  }
}


void Order_parser::parse(Processor& prc)
{
  It it = m_tokenizer.begin();
  set_tokens(it, m_tokenizer.end());

  if (!tokens_available())
    parse_error("Expected sorting order specification");

  Stored_any store_expr;

  Expr_parser_base parser(cur_pos(), end_pos(), m_mode);
  parser.process(store_expr);

  cdk::api::Sort_direction::value dir = cdk::api::Sort_direction::ASC;

  // get ASC/DESC token if available
  if (tokens_available())
  {
    switch(Keyword::get(*peek_token()))
    {
      case Keyword::ASC:
        consume_token();
        dir = cdk::api::Sort_direction::ASC;
        break;
      case Keyword::DESC:
        consume_token();
        dir = cdk::api::Sort_direction::DESC;
        break;
      default:
        parse_error("Expected sorting direction ASC or DESC");
    }
  }

  if (tokens_available())
    parse_error("Unexpected characters after sorting order specification");

  store_expr.process_if(prc.sort_key(dir));

}


void Projection_parser::parse_tbl_mode(Projection_processor& prc)
{
  It it = m_tokenizer.begin();
  set_tokens(it, m_tokenizer.end());

  if (!tokens_available())
    parse_error("Expected projection specification");

  Expr_parser_base parser(cur_pos(), end_pos(), m_mode);
  parser.process_if(prc.expr());

  // get AS token if available
  if (tokens_available())
  {
    if (!consume_token(Keyword::AS))
      parse_error("Invalid characters in projection specification,"
                  " only AS <name> allowed after the projection expression");

    if (!Token_base::cur_token_type_in({ Token::WORD, Token::QWORD }))
      parse_error("Expected identifier after AS");

    prc.alias(consume_token()->get_text());
  }


  if (tokens_available())
    parse_error("Unexpected characters after projection specification");
}


void Projection_parser::parse_doc_mode(Document_processor& prc)
{
  It it = m_tokenizer.begin();
  set_tokens(it, m_tokenizer.end());

  if (!tokens_available())
    parse_error("Expected projection specification");

  /*
    note: passing m_toks.end() directly as constructor argument results
    in "incompatible iterators" exception when comparing iterators (at
    least on win, vs2010). problem with passing temporary object?
  */

  Stored_any store_expr;

  Expr_parser_base parser(cur_pos(), end_pos(), m_mode);
  parser.process(store_expr);


  // AS is mandatory on Collections
  if (!consume_token(Keyword::AS))
    parse_error(L"Expected AS in projection specification");

  if (!Token_base::cur_token_type_in({Token::WORD,Token::QWORD}))
    parse_error(L"Expected identifier after AS");

  const string &id = consume_token()->get_text();

  if (tokens_available())
    parse_error(L"Invalid characters after projection specification");

  store_expr.process_if(prc.key_val(id));
}
