_( TEOF, "eof" )
_( TSEMI, ";" )

_( TLPAREN, "(" ) _( TRPAREN, ")" )
_( TLBRACE, "{" ) _( TRBRACE, "}" )
_( TLBRACK, "[" ) _( TRBRACK, "]" )

_( TDOT,        "." )
_( TDOTDOTDOT,  "..." )
_( TCOLON,      ":" )
_( TCOMMA,      "," )
_( TQUESTION,   "?" )

_( TPLUS,       "+" )
_( TPLUSPLUS,   "++" )
_( TMINUS,      "-" )
_( TMINUSMINUS, "--" )
_( TSTAR,       "*" )
_( TSLASH,      "/" )
_( TPERCENT,    "%" )
_( TTILDE,      "~" )
_( TNOT,        "!" )
_( TAND,        "&" )
_( TANDAND,     "&&" )
_( TOR,         "|" )
_( TOROR,       "||" )
_( TXOR,        "^" )
_( TSHL,        "<<" )
_( TSHR,        ">>" )

_( TEQ,   "==" )
_( TNEQ,  "!=" )

_( TLT,   "<" )
_( TGT,   ">" )
_( TLTEQ, "<=" )
_( TGTEQ, ">=" )

// assignment operators (if this changes, update tok_isassign)
_( TASSIGN,    "=" )
_( TADDASSIGN, "+=" )
_( TSUBASSIGN, "-=" )
_( TMULASSIGN, "*=" )
_( TDIVASSIGN, "/=" )
_( TMODASSIGN, "%=" )
_( TSHLASSIGN, "<<=" )
_( TSHRASSIGN, ">>=" )
_( TANDASSIGN, "&=" )
_( TXORASSIGN, "^=" )
_( TORASSIGN,  "|=" )

_( TCOMMENT, "comment" )
_( TID, "identifier" )
_( TINTLIT, "integer literal" )
_( TFLOATLIT, "number literal" )
_( TBYTELIT, "byte literal" )
_( TSTRLIT, "string literal" )

// keywords (must be sorted)
KEYWORD( "fun",      TFUN )
KEYWORD( "let",      TLET )
KEYWORD( "return",   TRETURN )
KEYWORD( "struct",   TSTRUCT )
KEYWORD( "type",     TTYPE )
KEYWORD( "var",      TVAR )
