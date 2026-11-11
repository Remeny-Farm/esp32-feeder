#include "feeder_util.h"

int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    /* Howard Hinnant's algorithm. March-based years put the leap day last. */
    if (m <= 2) {
        y -= 1;
    }
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t mp = (int64_t)(m > 2 ? m - 3 : m + 9);
    const int64_t doy = (153 * mp + 2) / 5 + (int64_t)d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t dd = doy - (153 * mp + 2) / 5 + 1;
    const int64_t mm = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400 + (mm <= 2 ? 1 : 0));
    *m = (int)mm;
    *d = (int)dd;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Read exactly n digits. A NUL stops the loop because it is not a digit. */
static bool read_digits(const char **p, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!is_digit((*p)[i])) {
            return false;
        }
        v = v * 10 + ((*p)[i] - '0');
    }
    *p += n;
    *out = v;
    return true;
}

static bool expect(const char **p, char c)
{
    if (**p != c) {
        return false;
    }
    (*p)++;
    return true;
}

static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in_month(int y, int m)
{
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && is_leap(y)) ? 29 : days[m - 1];
}

bool rfc3339_parse(const char *s, int64_t *epoch)
{
    if (s == NULL || epoch == NULL) {
        return false;
    }
    const char *p = s;
    int year, mon, day, hour, min, sec;
    if (!read_digits(&p, 4, &year) || !expect(&p, '-') || !read_digits(&p, 2, &mon) ||
        !expect(&p, '-') || !read_digits(&p, 2, &day) || !expect(&p, 'T') ||
        !read_digits(&p, 2, &hour) || !expect(&p, ':') || !read_digits(&p, 2, &min) ||
        !expect(&p, ':') || !read_digits(&p, 2, &sec)) {
        return false;
    }
    if (mon < 1 || mon > 12 || day < 1 || day > days_in_month(year, mon) || hour > 23 ||
        min > 59 || sec > 59) {
        return false;
    }
    if (*p == '.') {
        p++;
        if (!is_digit(*p)) {
            return false; /* a dot needs at least one digit */
        }
        while (is_digit(*p)) {
            p++;
        }
    }
    int64_t offset = 0;
    if (*p == 'Z') {
        p++;
    } else if (*p == '+' || *p == '-') {
        const int sign = (*p == '-') ? -1 : 1;
        int oh, om;
        p++;
        if (!read_digits(&p, 2, &oh) || !expect(&p, ':') || !read_digits(&p, 2, &om) || oh > 23 ||
            om > 59) {
            return false;
        }
        offset = (int64_t)sign * (oh * 3600 + om * 60);
    } else {
        return false; /* a zone is mandatory */
    }
    if (*p != '\0') {
        return false;
    }
    *epoch = days_from_civil(year, (unsigned)mon, (unsigned)day) * 86400 + hour * 3600 + min * 60 +
             sec - offset;
    return true;
}

static void put_digits(char *dst, int v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        dst[i] = (char)('0' + v % 10);
        v /= 10;
    }
}

size_t rfc3339_format(int64_t epoch, char *buf, size_t len)
{
    /* 0000-01-01T00:00:00Z and 9999-12-31T23:59:59Z as epoch seconds. */
    if (buf == NULL || len < RFC3339_UTC_BUF) {
        return 0;
    }
    if (epoch < INT64_C(-62167219200) || epoch > INT64_C(253402300799)) {
        buf[0] = '\0';
        return 0;
    }
    int64_t days = epoch / 86400;
    int64_t rem = epoch % 86400;
    if (rem < 0) { /* floor division for pre-1970 times */
        rem += 86400;
        days -= 1;
    }
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    put_digits(buf + 0, y, 4);
    buf[4] = '-';
    put_digits(buf + 5, m, 2);
    buf[7] = '-';
    put_digits(buf + 8, d, 2);
    buf[10] = 'T';
    put_digits(buf + 11, (int)(rem / 3600), 2);
    buf[13] = ':';
    put_digits(buf + 14, (int)(rem % 3600 / 60), 2);
    buf[16] = ':';
    put_digits(buf + 17, (int)(rem % 60), 2);
    buf[19] = 'Z';
    buf[20] = '\0';
    return 20;
}

bool hhmm_parse(const char *s, uint16_t *minute)
{
    if (s == NULL || minute == NULL) {
        return false;
    }
    const char *p = s;
    int h, m;
    if (!read_digits(&p, 2, &h) || !expect(&p, ':') || !read_digits(&p, 2, &m) || *p != '\0') {
        return false;
    }
    if (h > 23 || m > 59) {
        return false;
    }
    *minute = (uint16_t)(h * 60 + m);
    return true;
}

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool uuid_valid(const char *s)
{
    if (s == NULL) {
        return false;
    }
    for (int i = 0; i < 36; i++) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
        } else if (!is_hex(c)) {
            return false;
        }
    }
    return s[36] == '\0';
}
