static int square(int x) { return x * x; }

int sum_squares(const int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += square(a[i]);
  return s;
}

void scale(float *dst, const float *src, const float *k, int n) {
  for (int i = 0; i < n; ++i)
    dst[i] = src[i] * *k;
}

void scale_restrict(float *restrict dst, const float *restrict src,
                    const float *restrict k, int n) {
  for (int i = 0; i < n; ++i)
    dst[i] = src[i] * *k;
}

int classify(int x) {
  if (x < 0)
    return -1;
  if (x == 0)
    return 0;
  return 1;
}
