#include "amount.h"
#include "util.h"

#include <sstream>
#include <deque>

#include "gmp.h"

namespace ledger {

#define BIGINT_BULK_ALLOC 0x0001

class amount_t::bigint_t {
 public:
  mpz_t		 val;
  unsigned short prec;
  unsigned short flags;
  unsigned int	 ref;
  unsigned int	 index;

  bigint_t() : prec(0), flags(0), ref(1), index(0) {
    mpz_init(val);
  }
  bigint_t(mpz_t _val) : prec(0), flags(0), ref(1), index(0) {
    mpz_init_set(val, _val);
  }
  bigint_t(const bigint_t& other)
    : prec(other.prec), flags(0), ref(1), index(0) {
    mpz_init_set(val, other.val);
  }
  ~bigint_t() {
    assert(ref == 0);
    mpz_clear(val);
  }
};

unsigned int sizeof_bigint_t() {
  return sizeof(amount_t::bigint_t);
}

#define MPZ(x) ((x)->val)

static mpz_t		  temp;
static mpz_t		  divisor;
static amount_t::bigint_t true_value;

commodity_t::updater_t *  commodity_t::updater = NULL;
commodities_map		  commodity_t::commodities;
commodity_t *             commodity_t::null_commodity;

static struct _init_amounts {
  _init_amounts();
  ~_init_amounts();
} _init_obj;

_init_amounts::_init_amounts()
{
  mpz_init(temp);
  mpz_init(divisor);

  mpz_set_ui(true_value.val, 1);

  commodity_t::updater	      = NULL;
  commodity_t::null_commodity = commodity_t::find_commodity("", true);
}

_init_amounts::~_init_amounts()
{
  mpz_clear(divisor);
  mpz_clear(temp);

  if (commodity_t::updater) {
    delete commodity_t::updater;
    commodity_t::updater = NULL;
  }

  for (commodities_map::iterator i = commodity_t::commodities.begin();
       i != commodity_t::commodities.end();
       i++)
    delete (*i).second;

  commodity_t::commodities.clear();

  true_value.ref--;
}

static void mpz_round(mpz_t out, mpz_t value, int value_prec, int round_prec)
{
  // Round `value', with an encoding precision of `value_prec', to a
  // rounded value with precision `round_prec'.  Result is stored in
  // `out'.

  assert(value_prec > round_prec);

  mpz_t quotient;
  mpz_t remainder;

  mpz_init(quotient);
  mpz_init(remainder);

  mpz_ui_pow_ui(divisor, 10, value_prec - round_prec);
  mpz_tdiv_qr(quotient, remainder, value, divisor);
  mpz_divexact_ui(divisor, divisor, 10);
  mpz_mul_ui(divisor, divisor, 5);

  if (mpz_sgn(remainder) < 0) {
    mpz_neg(divisor, divisor);
    if (mpz_cmp(remainder, divisor) < 0) {
      mpz_ui_pow_ui(divisor, 10, value_prec - round_prec);
      mpz_add(remainder, divisor, remainder);
      mpz_ui_sub(remainder, 0, remainder);
      mpz_add(out, value, remainder);
    } else {
      mpz_sub(out, value, remainder);
    }
  } else {
    if (mpz_cmp(remainder, divisor) >= 0) {
      mpz_ui_pow_ui(divisor, 10, value_prec - round_prec);
      mpz_sub(remainder, divisor, remainder);
      mpz_add(out, value, remainder);
    } else {
      mpz_sub(out, value, remainder);
    }
  }
  mpz_clear(quotient);
  mpz_clear(remainder);

  // chop off the rounded bits
  mpz_ui_pow_ui(divisor, 10, value_prec - round_prec);
  mpz_tdiv_q(out, out, divisor);
}

amount_t::amount_t(const bool value)
{
  if (value) {
    quantity = &true_value;
    quantity->ref++;
  } else {
    quantity = NULL;
  }
  commodity_ = NULL;
}

amount_t::amount_t(const int value)
{
  if (value != 0) {
    quantity = new bigint_t;
    mpz_set_si(MPZ(quantity), value);
  } else {
    quantity  = NULL;
  }
  commodity_ = NULL;
}

amount_t::amount_t(const unsigned int value)
{
  if (value != 0) {
    quantity = new bigint_t;
    mpz_set_ui(MPZ(quantity), value);
  } else {
    quantity  = NULL;
  }
  commodity_ = NULL;
}

amount_t::amount_t(const double value)
{
  if (value != 0.0) {
    quantity = new bigint_t;
    mpz_set_d(MPZ(quantity), value);
    // jww (2004-08-20): How do I calculate this?
  } else {
    quantity  = NULL;
  }
  commodity_ = NULL;
}

void amount_t::_release()
{
  if (--quantity->ref == 0) {
    if (! (quantity->flags & BIGINT_BULK_ALLOC))
      delete quantity;
    else
      quantity->~bigint_t();
  }
}

void amount_t::_init()
{
  if (! quantity) {
    quantity = new bigint_t;
  }
  else if (quantity->ref > 1) {
    _release();
    quantity = new bigint_t;
  }
}

void amount_t::_dup()
{
  if (quantity->ref > 1) {
    bigint_t * q = new bigint_t(*quantity);
    _release();
    quantity = q;
  }
}

void amount_t::_copy(const amount_t& amt)
{
  if (quantity != amt.quantity) {
    if (quantity)
      _release();

    // Never maintain a pointer into a bulk allocation pool; such
    // pointers are not guaranteed to remain.
    if (amt.quantity->flags & BIGINT_BULK_ALLOC) {
      quantity = new bigint_t(*amt.quantity);
    } else {
      quantity = amt.quantity;
      quantity->ref++;
    }
  }
  commodity_ = amt.commodity_;
}

amount_t& amount_t::operator=(const std::string& value)
{
  std::istringstream str(value);
  parse(str);
  return *this;
}

amount_t& amount_t::operator=(const char * value)
{
  std::string	     valstr(value);
  std::istringstream str(valstr);
  parse(str);
  return *this;
}

// assignment operator
amount_t& amount_t::operator=(const amount_t& amt)
{
  if (this != &amt) {
    if (amt.quantity)
      _copy(amt);
    else if (quantity)
      _clear();
  }
  return *this;
}

amount_t& amount_t::operator=(const bool value)
{
  if (! value) {
    if (quantity)
      _clear();
  } else {
    commodity_ = NULL;
    if (quantity)
      _release();
    quantity = &true_value;
    quantity->ref++;
  }
  return *this;
}

amount_t& amount_t::operator=(const int value)
{
  if (value == 0) {
    if (quantity)
      _clear();
  } else {
    commodity_ = NULL;
    _init();
    mpz_set_si(MPZ(quantity), value);
  }
  return *this;
}

amount_t& amount_t::operator=(const unsigned int value)
{
  if (value == 0) {
    if (quantity)
      _clear();
  } else {
    commodity_ = NULL;
    _init();
    mpz_set_ui(MPZ(quantity), value);
  }
  return *this;
}

amount_t& amount_t::operator=(const double value)
{
  if (value == 0.0) {
    if (quantity)
      _clear();
  } else {
    commodity_ = NULL;
    _init();
    // jww (2004-08-20): How do I calculate precision?
    mpz_set_d(MPZ(quantity), value);
  }
  return *this;
}


void amount_t::_resize(unsigned int prec)
{
  assert(prec < 256);

  if (! quantity || prec == quantity->prec)
    return;

  _dup();

  if (prec < quantity->prec) {
    mpz_ui_pow_ui(divisor, 10, quantity->prec - prec);
    mpz_tdiv_q(MPZ(quantity), MPZ(quantity), divisor);
  } else {
    mpz_ui_pow_ui(divisor, 10, prec - quantity->prec);
    mpz_mul(MPZ(quantity), MPZ(quantity), divisor);
  }

  quantity->prec = prec;
}


amount_t& amount_t::operator+=(const amount_t& amt)
{
  if (! amt.quantity)
    return *this;

  if (! quantity) {
    _copy(amt);
    return *this;
  }

  _dup();

  if (commodity_ != amt.commodity_)
    throw amount_error("Adding amounts with different commodities");

  if (quantity->prec == amt.quantity->prec) {
    mpz_add(MPZ(quantity), MPZ(quantity), MPZ(amt.quantity));
  }
  else if (quantity->prec < amt.quantity->prec) {
    _resize(amt.quantity->prec);
    mpz_add(MPZ(quantity), MPZ(quantity), MPZ(amt.quantity));
  } else {
    amount_t temp = amt;
    temp._resize(quantity->prec);
    mpz_add(MPZ(quantity), MPZ(quantity), MPZ(temp.quantity));
  }

  return *this;
}

amount_t& amount_t::operator-=(const amount_t& amt)
{
  if (! amt.quantity)
    return *this;

  if (! quantity) {
    quantity  = new bigint_t(*amt.quantity);
    commodity_ = amt.commodity_;
    mpz_neg(MPZ(quantity), MPZ(quantity));
    return *this;
  }

  _dup();

  if (commodity_ != amt.commodity_)
    throw amount_error("Subtracting amounts with different commodities");

  if (quantity->prec == amt.quantity->prec) {
    mpz_sub(MPZ(quantity), MPZ(quantity), MPZ(amt.quantity));
  }
  else if (quantity->prec < amt.quantity->prec) {
    _resize(amt.quantity->prec);
    mpz_sub(MPZ(quantity), MPZ(quantity), MPZ(amt.quantity));
  } else {
    amount_t temp = amt;
    temp._resize(quantity->prec);
    mpz_sub(MPZ(quantity), MPZ(quantity), MPZ(temp.quantity));
  }

  return *this;
}

amount_t& amount_t::operator*=(const amount_t& amt)
{
  if (! amt.quantity || ! quantity)
    return *this;

  _dup();

  mpz_mul(MPZ(quantity), MPZ(quantity), MPZ(amt.quantity));
  quantity->prec += amt.quantity->prec;

  unsigned int comm_prec = commodity().precision;
  if (quantity->prec > comm_prec + 6U) {
    mpz_round(MPZ(quantity), MPZ(quantity), quantity->prec, comm_prec + 6U);
    quantity->prec = comm_prec + 6U;
  }

  return *this;
}

amount_t& amount_t::operator/=(const amount_t& amt)
{
  if (! quantity)
    return *this;
  if (! amt.quantity)
    throw amount_error("Divide by zero");

  _dup();

  // Increase the value's precision, to capture fractional parts after
  // the divide.
  mpz_ui_pow_ui(divisor, 10, amt.quantity->prec + 6);
  mpz_mul(MPZ(quantity), MPZ(quantity), divisor);
  mpz_tdiv_q(MPZ(quantity), MPZ(quantity), MPZ(amt.quantity));
  quantity->prec += 6;

  unsigned int comm_prec = commodity().precision;
  if (quantity->prec > comm_prec + 6U) {
    mpz_round(MPZ(quantity), MPZ(quantity), quantity->prec, comm_prec + 6U);
    quantity->prec = comm_prec + 6U;
  }

  return *this;
}

// unary negation
void amount_t::negate()
{
  if (quantity) {
    _dup();
    mpz_neg(MPZ(quantity), MPZ(quantity));
  }
}

int amount_t::sign() const
{
  return quantity ? mpz_sgn(MPZ(quantity)) : 0;
}

// comparisons between amounts
#define AMOUNT_CMP_AMOUNT(OP)					\
bool amount_t::operator OP(const amount_t& amt) const		\
{								\
  if (! quantity)						\
    return amt > 0;						\
  if (! amt.quantity)						\
    return *this < 0;						\
								\
  if (commodity() && amt.commodity() &&				\
      commodity() != amt.commodity())                           \
    return false;						\
								\
  if (quantity->prec == amt.quantity->prec) {			\
    return mpz_cmp(MPZ(quantity), MPZ(amt.quantity)) OP 0;	\
  }								\
  else if (quantity->prec < amt.quantity->prec) {		\
    amount_t temp = *this;					\
    temp._resize(amt.quantity->prec);				\
    return mpz_cmp(MPZ(temp.quantity), MPZ(amt.quantity)) OP 0;	\
  }								\
  else {							\
    amount_t temp = amt;					\
    temp._resize(quantity->prec);				\
    return mpz_cmp(MPZ(quantity), MPZ(temp.quantity)) OP 0;	\
  }								\
}

AMOUNT_CMP_AMOUNT(<)
AMOUNT_CMP_AMOUNT(<=)
AMOUNT_CMP_AMOUNT(>)
AMOUNT_CMP_AMOUNT(>=)
AMOUNT_CMP_AMOUNT(==)

amount_t::operator bool() const
{
  if (! quantity)
    return false;

  if (quantity->prec <= commodity().precision) {
    return mpz_sgn(MPZ(quantity)) != 0;
  } else {
    assert(commodity_);
    mpz_set(temp, MPZ(quantity));
    mpz_ui_pow_ui(divisor, 10, quantity->prec - commodity().precision);
    mpz_tdiv_q(temp, temp, divisor);
    bool zero = mpz_sgn(temp) == 0;
    return ! zero;
  }
}

amount_t amount_t::value(const std::time_t moment) const
{
  if (quantity && ! (commodity().flags & COMMODITY_STYLE_NOMARKET))
    if (amount_t amt = commodity().value(moment))
      return (amt * *this).round(commodity().precision);

  return *this;
}

amount_t amount_t::round(unsigned int prec) const
{
  if (! quantity || quantity->prec <= prec)
    return *this;

  amount_t temp = *this;
  temp._dup();

  mpz_round(MPZ(temp.quantity), MPZ(temp.quantity), temp.quantity->prec, prec);
  temp.quantity->prec = prec;

  return temp;
}

std::ostream& operator<<(std::ostream& _out, const amount_t& amt)
{
  if (! amt.quantity)
    return _out;

  std::ostringstream out;

  mpz_t quotient;
  mpz_t rquotient;
  mpz_t remainder;

  mpz_init(quotient);
  mpz_init(rquotient);
  mpz_init(remainder);

  bool negative = false;

  // Ensure the value is rounded to the commodity's precision before
  // outputting it.  NOTE: `rquotient' is used here as a temp variable!

  commodity_t&   commodity(amt.commodity());
  unsigned short precision;

  if (commodity == *commodity_t::null_commodity) {
    mpz_ui_pow_ui(divisor, 10, amt.quantity->prec);
    mpz_tdiv_qr(quotient, remainder, MPZ(amt.quantity), divisor);
    precision = amt.quantity->prec;
  }
  else if (commodity.precision < amt.quantity->prec) {
    mpz_round(rquotient, MPZ(amt.quantity), amt.quantity->prec,
	      commodity.precision);
    mpz_ui_pow_ui(divisor, 10, commodity.precision);
    mpz_tdiv_qr(quotient, remainder, rquotient, divisor);
    precision = commodity.precision;
  }
  else if (commodity.precision > amt.quantity->prec) {
    mpz_ui_pow_ui(divisor, 10, commodity.precision - amt.quantity->prec);
    mpz_mul(rquotient, MPZ(amt.quantity), divisor);
    mpz_ui_pow_ui(divisor, 10, commodity.precision);
    mpz_tdiv_qr(quotient, remainder, rquotient, divisor);
    precision = commodity.precision;
  }
  else if (amt.quantity->prec) {
    mpz_ui_pow_ui(divisor, 10, amt.quantity->prec);
    mpz_tdiv_qr(quotient, remainder, MPZ(amt.quantity), divisor);
    precision = amt.quantity->prec;
  }
  else {
    mpz_set(quotient, MPZ(amt.quantity));
    mpz_set_ui(remainder, 0);
    precision = 0;
  }

  if (mpz_sgn(quotient) < 0 || mpz_sgn(remainder) < 0) {
    negative = true;

    mpz_abs(quotient, quotient);
    mpz_abs(remainder, remainder);
  }
  mpz_set(rquotient, remainder);

  if (! (commodity.flags & COMMODITY_STYLE_SUFFIXED)) {
    if (commodity.quote)
      out << "\"" << commodity.symbol << "\"";
    else
      out << commodity.symbol;
    if (commodity.flags & COMMODITY_STYLE_SEPARATED)
      out << " ";
  }

  if (negative)
    out << "-";

  if (mpz_sgn(quotient) == 0) {
    out << '0';
  }
  else if (! (commodity.flags & COMMODITY_STYLE_THOUSANDS)) {
    char * p = mpz_get_str(NULL, 10, quotient);
    out << p;
    std::free(p);
  }
  else {
    std::deque<std::string> strs;
    char buf[4];

    for (int powers = 0; true; powers += 3) {
      if (powers > 0) {
	mpz_ui_pow_ui(divisor, 10, powers);
	mpz_tdiv_q(temp, quotient, divisor);
	if (mpz_sgn(temp) == 0)
	  break;
	mpz_tdiv_r_ui(temp, temp, 1000);
      } else {
	mpz_tdiv_r_ui(temp, quotient, 1000);
      }
      mpz_get_str(buf, 10, temp);
      strs.push_back(buf);
    }

    bool printed = false;

    for (std::deque<std::string>::reverse_iterator i = strs.rbegin();
	 i != strs.rend();
	 i++) {
      if (printed) {
	out << (commodity.flags & COMMODITY_STYLE_EUROPEAN ? '.' : ',');
	out.width(3);
	out.fill('0');
      }
      out << *i;

      printed = true;
    }
  }

  if (precision) {
    out << ((commodity.flags & COMMODITY_STYLE_EUROPEAN) ? ',' : '.');

    out.width(precision);
    out.fill('0');

    char * p = mpz_get_str(NULL, 10, rquotient);
    out << p;
    std::free(p);
  }

  if (commodity.flags & COMMODITY_STYLE_SUFFIXED) {
    if (commodity.flags & COMMODITY_STYLE_SEPARATED)
      out << " ";
    if (commodity.quote)
      out << "\"" << commodity.symbol << "\"";
    else
      out << commodity.symbol;
  }

  mpz_clear(quotient);
  mpz_clear(rquotient);
  mpz_clear(remainder);

  // Things are output to a string first, so that if anyone has
  // specified a width or fill for _out, it will be applied to the
  // entire amount string, and not just the first part.

  _out << out.str();

  return _out;
}

void parse_quantity(std::istream& in, std::string& value)
{
  static char buf[256];
  char c = peek_next_nonws(in);
  READ_INTO(in, buf, 256, c,
	    std::isdigit(c) || c == '-' || c == '.' || c == ',');
  value = buf;
}

void parse_commodity(std::istream& in, std::string& symbol)
{
  static char buf[256];

  char c = peek_next_nonws(in);
  if (c == '"') {
    in.get(c);
    READ_INTO(in, buf, 256, c, c != '"');
    if (c == '"')
      in.get(c);
    else
      throw amount_error("Quoted commodity symbol lacks closing quote");
  } else {
    READ_INTO(in, buf, 256, c, ! std::isspace(c) && ! std::isdigit(c) &&
	      c != '-' && c != '.');
  }
  symbol = buf;
}

void amount_t::parse(std::istream& in)
{
  // The possible syntax for an amount is:
  //
  //   [-]NUM[ ]SYM [@ AMOUNT]
  //   SYM[ ][-]NUM [@ AMOUNT]

  std::string  symbol;
  std::string  quant;
  unsigned int flags = COMMODITY_STYLE_DEFAULTS;;

  char c = peek_next_nonws(in);
  if (std::isdigit(c) || c == '.' || c == '-') {
    parse_quantity(in, quant);

    char n;
    if (! in.eof() && ((n = in.peek()) != '\n')) {
      if (std::isspace(n))
	flags |= COMMODITY_STYLE_SEPARATED;

      parse_commodity(in, symbol);

      flags |= COMMODITY_STYLE_SUFFIXED;
    }
  } else {
    parse_commodity(in, symbol);

    if (std::isspace(in.peek()))
      flags |= COMMODITY_STYLE_SEPARATED;

    parse_quantity(in, quant);
  }

  if (quant.empty())
    throw amount_error("No quantity specified for amount");

  _init();

  std::string::size_type last_comma  = quant.rfind(',');
  std::string::size_type last_period = quant.rfind('.');

  if (last_comma != std::string::npos && last_period != std::string::npos) {
    flags |= COMMODITY_STYLE_THOUSANDS;
    if (last_comma > last_period) {
      flags |= COMMODITY_STYLE_EUROPEAN;
      quantity->prec = quant.length() - last_comma - 1;
    } else {
      quantity->prec = quant.length() - last_period - 1;
    }
  }
  else if (last_comma != std::string::npos) {
    flags |= COMMODITY_STYLE_EUROPEAN;
    quantity->prec = quant.length() - last_comma - 1;
  }
  else if (last_period != std::string::npos) {
    quantity->prec = quant.length() - last_period - 1;
  }
  else {
    quantity->prec = 0;
  }

  // Create the commodity if has not already been seen, and update the
  // precision if something greater was used for the quantity.

  commodity_ = commodity_t::find_commodity(symbol, true);
  commodity_->flags |= flags;
  if (quantity->prec > commodity_->precision)
    commodity_->precision = quantity->prec;

  // Now we have the final number.  Remove commas and periods, if
  // necessary.

  if (last_comma != std::string::npos || last_period != std::string::npos) {
    int	 len	 = quant.length();
    char * buf	 = new char[len + 1];

    const char * p = quant.c_str();
    char * t       = buf;

    while (*p) {
      if (*p == ',' || *p == '.')
	p++;
      *t++ = *p++;
    }
    *t = '\0';

    mpz_set_str(MPZ(quantity), buf, 10);

    delete[] buf;
  } else {
    mpz_set_str(MPZ(quantity), quant.c_str(), 10);
  }
}

void amount_t::parse(const std::string& str)
{
  std::istringstream stream(str);
  parse(stream);
}


char *	     bigints;
char *	     bigints_next;
unsigned int bigints_index;
unsigned int bigints_count;

void amount_t::read_quantity(char *& data)
{
  char byte = *data++;;

  if (byte == 0) {
    quantity = NULL;
  }
  else if (byte == 1) {
    quantity = new((bigint_t *)bigints_next) bigint_t;
    bigints_next += sizeof(bigint_t);
    quantity->flags |= BIGINT_BULK_ALLOC;

    unsigned short len = *((unsigned short *) data);
    data += sizeof(unsigned short);
    mpz_import(MPZ(quantity), len / sizeof(short), 1, sizeof(short),
	       0, 0, data);
    data += len;

    char negative = *data++;
    if (negative)
      mpz_neg(MPZ(quantity), MPZ(quantity));

    quantity->prec = *((unsigned short *) data);
    data += sizeof(unsigned short);
  } else {
    unsigned int index = *((unsigned int *) data);
    data += sizeof(unsigned int);

    quantity = (bigint_t *) (bigints + (index - 1) * sizeof(bigint_t));
    quantity->ref++;
  }
}

static char buf[4096];

void amount_t::read_quantity(std::istream& in)
{
  static std::deque<bigint_t *> _bigints;

  char byte;
  in.read(&byte, sizeof(byte));

  if (byte == 0) {
    quantity = NULL;
  }
  else if (byte == 1) {
    quantity = new bigint_t;
    _bigints.push_back(quantity);

    unsigned short len;
    in.read((char *)&len, sizeof(len));
    assert(len < 4096);
    in.read(buf, len);
    mpz_import(MPZ(quantity), len / sizeof(short), 1, sizeof(short),
	       0, 0, buf);

    char negative;
    in.read(&negative, sizeof(negative));
    if (negative)
      mpz_neg(MPZ(quantity), MPZ(quantity));

    in.read((char *)&quantity->prec, sizeof(quantity->prec));
  } else {
    unsigned int index;
    in.read((char *)&index, sizeof(index));
    quantity = _bigints[index - 1];
    quantity->ref++;
  }
}

void amount_t::write_quantity(std::ostream& out) const
{
  char byte;

  if (! quantity) {
    byte = 0;
    out.write(&byte, sizeof(byte));
    return;
  }

  if (quantity->index == 0) {
    quantity->index = ++bigints_index;
    bigints_count++;

    byte = 1;
    out.write(&byte, sizeof(byte));

    std::size_t size;
    mpz_export(buf, &size, 1, sizeof(short), 0, 0, MPZ(quantity));
    unsigned short len = size * sizeof(short);
    out.write((char *)&len, sizeof(len));
    if (len) {
      assert(len < 4096);
      out.write(buf, len);
    }

    byte = mpz_sgn(MPZ(quantity)) < 0 ? 1 : 0;
    out.write(&byte, sizeof(byte));

    out.write((char *)&quantity->prec, sizeof(quantity->prec));
  } else {
    assert(quantity->ref > 1);

    // Since this value has already been written, we simply write
    // out a reference to which one it was.
    byte = 2;
    out.write(&byte, sizeof(byte));
    out.write((char *)&quantity->index, sizeof(quantity->index));
  }
}

bool amount_t::valid() const
{
  if (quantity) {
    if (! commodity_)
      return false;

    if (quantity->ref == 0)
      return false;
  }
  else if (commodity_) {
    return false;
  }

  return true;
}


void commodity_t::add_price(const std::time_t date, const amount_t& price)
{
  history_map::iterator i = history.find(date);
  if (i != history.end()) {
    (*i).second = price;
  } else {
    std::pair<history_map::iterator, bool> result
      = history.insert(history_pair(date, price));
    assert(result.second);
  }
}

commodity_t * commodity_t::find_commodity(const std::string& symbol,
					  bool auto_create)
{
  commodities_map::const_iterator i = commodities.find(symbol);
  if (i != commodities.end())
    return (*i).second;

  if (auto_create) {
    commodity_t * commodity = new commodity_t(symbol);
    add_commodity(commodity);
    return commodity;
  }

  return NULL;
}

amount_t commodity_t::value(const std::time_t moment)
{
  std::time_t age = 0;
  amount_t    price;

  for (history_map::reverse_iterator i = history.rbegin();
       i != history.rend();
       i++)
    if (moment == 0 || std::difftime(moment, (*i).first) >= 0) {
      age   = (*i).first;
      price = (*i).second;
      break;
    }

  if (updater)
    (*updater)(*this, moment, age, (history.size() > 0 ?
				    (*history.rbegin()).first : 0), price);
  return price;
}

} // namespace ledger

#ifdef USE_BOOST_PYTHON

#include <boost/python.hpp>
#include <Python.h>

using namespace boost::python;
using namespace ledger;

void (amount_t::*parse_1)(std::istream& in)       = &amount_t::parse;
void (amount_t::*parse_2)(const std::string& str) = &amount_t::parse;

struct commodity_updater_wrap : public commodity_t::updater_t
{
  PyObject * self;
  commodity_updater_wrap(PyObject * self_) : self(self_) {}

  virtual void operator()(commodity_t&      commodity,
			  const std::time_t moment,
			  const std::time_t date,
			  const std::time_t last,
			  amount_t&         price) {
    call_method<void>(self, "__call__", commodity, moment, date, last, price);
  }
};

commodity_t * py_find_commodity_1(const std::string& symbol)
{
  return commodity_t::find_commodity(symbol);
}

commodity_t * py_find_commodity_2(const std::string& symbol, bool auto_create)
{
  return commodity_t::find_commodity(symbol, auto_create);
}

void export_amount()
{
  class_< amount_t > ("Amount")
    .def(init<amount_t>())
    .def(init<std::string>())
    .def(init<char *>())
    .def(init<bool>())
    .def(init<int>())
    .def(init<unsigned int>())
    .def(init<double>())

    .def("commodity", &amount_t::commodity,
	 return_value_policy<reference_existing_object>())
    .def("set_commodity", &amount_t::set_commodity)
    .def("clear_commodity", &amount_t::clear_commodity)

    .def(self += self)
    .def(self += int())
    .def(self +  self)
    .def(self +  int())
    .def(self -= self)
    .def(self -= int())
    .def(self -  self)
    .def(self -  int())
    .def(self *= self)
    .def(self *= int())
    .def(self *  self)
    .def(self *  int())
    .def(self /= self)
    .def(self /= int())
    .def(self /  self)
    .def(self /  int())
    .def(- self)

    .def(self <  self)
    .def(self <  int())
    .def(self <= self)
    .def(self <= int())
    .def(self >  self)
    .def(self >  int())
    .def(self >= self)
    .def(self >= int())
    .def(self == self)
    .def(self == int())
    .def(self != self)
    .def(self != int())
    .def(! self)

    .def(abs(self))
    .def(self_ns::str(self))

    .def("negate", &amount_t::negate)
    .def("parse", parse_1)
    .def("parse", parse_2)
    .def("valid", &amount_t::valid)
    ;

  class_< commodity_t::updater_t, commodity_updater_wrap, boost::noncopyable >
    ("Updater")
    ;

  scope().attr("COMMODITY_STYLE_DEFAULTS")  = COMMODITY_STYLE_DEFAULTS;
  scope().attr("COMMODITY_STYLE_SUFFIXED")  = COMMODITY_STYLE_SUFFIXED;
  scope().attr("COMMODITY_STYLE_SEPARATED") = COMMODITY_STYLE_SEPARATED;
  scope().attr("COMMODITY_STYLE_EUROPEAN")  = COMMODITY_STYLE_EUROPEAN;
  scope().attr("COMMODITY_STYLE_THOUSANDS") = COMMODITY_STYLE_THOUSANDS;
  scope().attr("COMMODITY_STYLE_NOMARKET")  = COMMODITY_STYLE_NOMARKET;

  class_< commodity_t > ("Commodity")
    .def(init<std::string, optional<unsigned int, unsigned int> >())

    .def_readonly("symbol", &commodity_t::symbol)
    .def("set_symbol", &commodity_t::set_symbol)
    .def_readwrite("name", &commodity_t::name)
    .def_readwrite("note", &commodity_t::name)
    .def_readwrite("precision", &commodity_t::precision)
    .def_readwrite("flags", &commodity_t::flags)
    .def_readwrite("last_lookup", &commodity_t::last_lookup)
    .def_readwrite("conversion", &commodity_t::conversion)
    .def_readwrite("ident", &commodity_t::ident)
    .def_readwrite("updater", &commodity_t::updater)

    .def(self_ns::str(self))

    .def("add_price", &commodity_t::add_price)
    .def("remove_price", &commodity_t::remove_price)
    .def("value", &commodity_t::value)

    .def("valid", &commodity_t::valid)
    ;

  def("add_commodity", &commodity_t::add_commodity);
  def("remove_commodity", &commodity_t::remove_commodity);
  def("find_commodity", py_find_commodity_1,
      return_value_policy<reference_existing_object>());
  def("find_commodity", py_find_commodity_2,
      return_value_policy<reference_existing_object>());
}

#endif // USE_BOOST_PYTHON
