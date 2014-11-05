#define F (1 << 14)
#define INT_MAX ((1 << 31) - 1)
#define INT_MIN (-(1 << 31))

int int_to_fp(int n);
int fp_to_int_round(int x);
int fp_to_int(int x);
int add_fp(int x, int y);
int add_mixed(int x, int n);
int sub_fp(int x, int y);
int sub_mixed(int x, int n);
int mult_fp(int x, int y);
int mult_mixed(int x, int y);
int div_fp(int x, int y);
int div_mixed(int x, int n);


int int_to_fp(int n)
{
  return n * F;
}
int fp_to_int_round(int x)
{
  return x / F;
}
int fp_to_int(int x)
{
  return x / F;
}
int add_fp(int x, int y)
{
  return x + y;
}
int add_mixed(int x, int n)
{
  return add_fp(x, int_to_fp(n));
}
int sub_fp(int x, int y)
{
  return x - y;
}
int sub_mixed(int x, int n)
{
  return sub_fp(x, int_to_fp(n));
}
int mult_fp(int x, int y)
{
  return (int)((long long )x * y / F);
}
int mult_mixed(int x, int y)
{
  return mult_fp(x, int_to_fp(y));
}
int div_fp(int x, int y)
{
  return (long long)x * F / y;
}
int div_mixed(int x, int n)
{
  return div_fp(x, int_to_fp(n));
}
