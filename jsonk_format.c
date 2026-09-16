/* JSON Schema "format" assertions. Per spec, "format" is annotation-only
 * unless a validator opts into asserting it -- jsonk asserts the common
 * ones real-world schemas actually use. These are deliberately pragmatic,
 * structural checks (not exhaustive RFC-grammar parsers): good enough to
 * catch genuinely malformed values without becoming their own maintenance
 * burden. See CLAUDE.md for exactly what's simplified in each case. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_jsonk.h"
#include "jsonk_format.h"
#include "jsonk_regex.h"
#include <string.h>

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static bool parse_2digit(const char *s, int *out)
{
	if (!is_digit(s[0]) || !is_digit(s[1])) return false;
	*out = (s[0] - '0') * 10 + (s[1] - '0');
	return true;
}

static bool is_leap_year(int y)
{
	return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
	static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (m < 1 || m > 12) return 0;
	if (m == 2 && is_leap_year(y)) return 29;
	return days[m - 1];
}

/* RFC 3339 full-date: YYYY-MM-DD */
static bool check_date(const char *s, size_t len)
{
	int year, month, day;

	if (len != 10) return false;
	if (!is_digit(s[0]) || !is_digit(s[1]) || !is_digit(s[2]) || !is_digit(s[3])) return false;
	if (s[4] != '-') return false;
	if (!parse_2digit(s + 5, &month)) return false;
	if (s[7] != '-') return false;
	if (!parse_2digit(s + 8, &day)) return false;

	year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
	if (month < 1 || month > 12) return false;
	if (day < 1 || day > days_in_month(year, month)) return false;
	return true;
}

/* RFC 3339 partial-time: HH:MM:SS(.fraction)?(Z|+HH:MM|-HH:MM) */
static bool check_time(const char *s, size_t len)
{
	int hour, minute, second;
	size_t i;

	if (len < 9) return false; /* shortest valid form: "HH:MM:SSZ" */
	if (!parse_2digit(s, &hour)) return false;
	if (s[2] != ':') return false;
	if (!parse_2digit(s + 3, &minute)) return false;
	if (s[5] != ':') return false;
	if (!parse_2digit(s + 6, &second)) return false;
	if (hour > 23 || minute > 59 || second > 60) return false; /* 60 allows a leap second */

	i = 8;
	if (i < len && s[i] == '.') {
		size_t start = ++i;
		while (i < len && is_digit(s[i])) i++;
		if (i == start) return false; /* '.' with no digits after it */
	}

	if (i >= len) return false; /* a timezone (Z or offset) is required */
	if (s[i] == 'Z' || s[i] == 'z') {
		return i + 1 == len;
	}
	if (s[i] == '+' || s[i] == '-') {
		int off_h, off_m;
		i++;
		if (len - i != 5) return false;
		if (!parse_2digit(s + i, &off_h)) return false;
		if (s[i + 2] != ':') return false;
		if (!parse_2digit(s + i + 3, &off_m)) return false;
		return off_h <= 23 && off_m <= 59;
	}
	return false;
}

static bool check_date_time(const char *s, size_t len)
{
	if (len < 11) return false;
	if (s[10] != 'T' && s[10] != 't') return false;
	return check_date(s, 10) && check_time(s + 11, len - 11);
}

/* ISO 8601 duration: PnYnMnDTnHnMnS or the PnW shorthand. A structural
 * check (component order/units), not calendar-aware validation. */
static bool check_duration(const char *s, size_t len)
{
	size_t i;
	bool has_component = false;
	bool in_time_part = false;
	bool last_was_unit = false;

	if (len < 3 || s[0] != 'P') return false;

	if (is_digit(s[1])) {
		size_t j = 1;
		while (j < len && is_digit(s[j])) j++;
		if (j < len && s[j] == 'W') {
			return j + 1 == len; /* PnW: weeks only, nothing else combinable */
		}
	}

	i = 1;
	while (i < len) {
		if (s[i] == 'T') {
			if (in_time_part) return false;
			in_time_part = true;
			last_was_unit = false;
			i++;
			continue;
		}
		if (!is_digit(s[i])) return false;
		{
			size_t start = i;
			char unit;
			while (i < len && is_digit(s[i])) i++;
			if (i == start || i >= len) return false;
			unit = s[i++];
			if (!in_time_part) {
				if (unit != 'Y' && unit != 'M' && unit != 'D') return false;
			} else {
				if (unit != 'H' && unit != 'M' && unit != 'S') return false;
			}
			has_component = true;
			last_was_unit = true;
		}
	}
	return has_component && last_was_unit; /* rejects a bare "PT" with no components */
}

static bool check_hostname(const char *s, size_t len)
{
	size_t label_start = 0, i;

	if (len == 0 || len > 253) return false;

	for (i = 0; i <= len; i++) {
		if (i == len || s[i] == '.') {
			size_t label_len = i - label_start;
			size_t j;
			if (label_len == 0 || label_len > 63) return false;
			if (s[label_start] == '-' || s[i - 1] == '-') return false;
			for (j = label_start; j < i; j++) {
				char c = s[j];
				bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
				if (!ok) return false;
			}
			label_start = i + 1;
		}
	}
	return true;
}

static bool check_ipv4(const char *s, size_t len)
{
	int octet_count = 0;
	size_t i = 0;

	while (i < len) {
		size_t start = i;
		int value = 0;
		int digit_count = 0;
		while (i < len && is_digit(s[i])) {
			digit_count++;
			if (digit_count > 3) return false;
			value = value * 10 + (s[i] - '0');
			i++;
		}
		if (digit_count == 0) return false;
		if (digit_count > 1 && s[start] == '0') return false; /* no leading zeros */
		if (value > 255) return false;
		octet_count++;
		if (i < len) {
			if (s[i] != '.') return false;
			i++;
		}
	}
	return octet_count == 4;
}

/* Structural check (segment count/hex validity + "::" compression), not a
 * full RFC 4291 canonicalizer. Accepts an embedded IPv4 tail
 * (e.g. "::ffff:192.0.2.1"). */
static bool check_ipv6(const char *s, size_t len)
{
	int groups = 0;
	bool has_double_colon = false;
	size_t seg_start = 0, j;

	if (len == 0 || len > 45) return false;

	for (j = 0; j <= len; j++) {
		if (j != len && s[j] != ':') continue;

		{
			size_t seg_len = j - seg_start;
			if (seg_len == 0) {
				if (j == 0 || j == len) {
					/* one side of a leading/trailing "::" -- the other
					 * side (an adjacent empty segment) is what actually
					 * flags has_double_colon below. */
				} else if (has_double_colon) {
					return false; /* more than one "::" */
				} else {
					has_double_colon = true;
				}
			} else {
				size_t k;
				if (memchr(s + seg_start, '.', seg_len)) {
					if (j != len || !check_ipv4(s + seg_start, seg_len)) return false;
					groups += 2;
					seg_start = j + 1;
					continue;
				}
				if (seg_len > 4) return false;
				for (k = seg_start; k < j; k++) {
					char c = s[k];
					bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
					if (!hex) return false;
				}
				groups++;
			}
			seg_start = j + 1;
		}
	}

	return has_double_colon ? (groups <= 7) : (groups == 8);
}

static bool check_email(const char *s, size_t len)
{
	size_t at = 0, i;
	bool found_at = false;
	size_t domain_start, domain_len;
	bool has_dot = false;

	if (len == 0) return false;

	for (i = 0; i < len; i++) {
		if (s[i] == '@') {
			if (found_at) return false;
			found_at = true;
			at = i;
		}
	}
	if (!found_at || at == 0 || at == len - 1) return false;

	for (i = 0; i < at; i++) {
		unsigned char c = (unsigned char) s[i];
		if (c <= 0x20 || c == 0x7f) return false;
	}
	if (s[0] == '.' || s[at - 1] == '.') return false;
	for (i = 1; i < at; i++) {
		if (s[i] == '.' && s[i - 1] == '.') return false;
	}

	domain_start = at + 1;
	domain_len = len - domain_start;
	if (!check_hostname(s + domain_start, domain_len)) return false;
	for (i = domain_start; i < len; i++) {
		if (s[i] == '.') has_dot = true;
	}
	return has_dot;
}

static bool has_valid_scheme(const char *s, size_t len, size_t *scheme_end)
{
	size_t i;
	if (len == 0) return false;
	if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) return false;
	for (i = 1; i < len; i++) {
		char c = s[i];
		if (c == ':') { *scheme_end = i; return true; }
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) {
			return false;
		}
	}
	return false;
}

static bool check_uri(const char *s, size_t len)
{
	size_t scheme_end, i;
	if (!has_valid_scheme(s, len, &scheme_end)) return false;
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char) s[i];
		if (c <= 0x20 || c == 0x7f) return false;
	}
	return true;
}

static bool check_uri_reference(const char *s, size_t len)
{
	size_t i;
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char) s[i];
		if (c <= 0x20 || c == 0x7f) return false;
	}
	return true; /* an empty string is a valid (empty) relative reference */
}

static bool check_uuid(const char *s, size_t len)
{
	static const char pattern[] = "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$";
	return jsonk_regex_match(pattern, sizeof(pattern) - 1, s, len);
}

static bool check_json_pointer(const char *s, size_t len)
{
	size_t i;
	if (len == 0) return true; /* the empty pointer refers to the whole document */
	if (s[0] != '/') return false;
	for (i = 0; i < len; i++) {
		if (s[i] == '~' && (i + 1 >= len || (s[i + 1] != '0' && s[i + 1] != '1'))) {
			return false;
		}
	}
	return true;
}

static bool check_relative_json_pointer(const char *s, size_t len)
{
	size_t i = 0;
	if (len == 0 || !is_digit(s[0])) return false;
	while (i < len && is_digit(s[i])) i++;
	if (i == len) return true;
	if (s[i] == '#') return i + 1 == len;
	return check_json_pointer(s + i, len - i);
}

bool jsonk_check_format(const char *format, size_t format_len, const char *value, size_t value_len)
{
#define JSONK_FMT_IS(name) (format_len == sizeof(name) - 1 && memcmp(format, name, format_len) == 0)

	if (JSONK_FMT_IS("date-time"))             return check_date_time(value, value_len);
	if (JSONK_FMT_IS("date"))                  return check_date(value, value_len);
	if (JSONK_FMT_IS("time"))                  return check_time(value, value_len);
	if (JSONK_FMT_IS("duration"))              return check_duration(value, value_len);
	if (JSONK_FMT_IS("email"))                 return check_email(value, value_len);
	if (JSONK_FMT_IS("idn-email"))             return check_email(value, value_len);
	if (JSONK_FMT_IS("hostname"))              return check_hostname(value, value_len);
	if (JSONK_FMT_IS("idn-hostname"))          return check_hostname(value, value_len);
	if (JSONK_FMT_IS("ipv4"))                  return check_ipv4(value, value_len);
	if (JSONK_FMT_IS("ipv6"))                  return check_ipv6(value, value_len);
	if (JSONK_FMT_IS("uri"))                   return check_uri(value, value_len);
	if (JSONK_FMT_IS("iri"))                   return check_uri(value, value_len);
	if (JSONK_FMT_IS("uri-reference"))         return check_uri_reference(value, value_len);
	if (JSONK_FMT_IS("iri-reference"))         return check_uri_reference(value, value_len);
	if (JSONK_FMT_IS("uuid"))                  return check_uuid(value, value_len);
	if (JSONK_FMT_IS("json-pointer"))          return check_json_pointer(value, value_len);
	if (JSONK_FMT_IS("relative-json-pointer")) return check_relative_json_pointer(value, value_len);
	if (JSONK_FMT_IS("regex"))                 return jsonk_regex_is_valid(value, value_len);

#undef JSONK_FMT_IS

	/* Unrecognized format name (e.g. "uri-template", or a dialect-specific
	 * extension): annotation-only, per spec default -- not a failure. */
	return true;
}
