/*
 * This file implements string parsing and creation for NumPy datetime.
 *
 * Written by Mark Wiebe (mwwiebe@gmail.com)
 * Copyright (c) 2011 by Enthought, Inc.
 *
 * See LICENSE.txt for the license.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <time.h>

#define _MULTIARRAYMODULE
#include <numpy/arrayobject.h>

#include "npy_config.h"
#include "numpy/npy_3kcompat.h"

#include "numpy/arrayscalars.h"
#include "methods.h"
#include "_datetime.h"
#include "datetime_strings.h"

/*
 * Parses (almost) standard ISO 8601 date strings. The differences are:
 *
 * + After the date and time, may place a ' ' followed by an event number.
 * + The date "20100312" is parsed as the year 20100312, not as
 *   equivalent to "2010-03-12". The '-' in the dates are not optional.
 * + Only seconds may have a decimal point, with up to 18 digits after it
 *   (maximum attoseconds precision).
 * + Either a 'T' as in ISO 8601 or a ' ' may be used to separate
 *   the date and the time. Both are treated equivalently.
 * + Doesn't (yet) handle the "YYYY-DDD" or "YYYY-Www" formats.
 * + Doesn't handle leap seconds (seconds value has 60 in these cases).
 * + Doesn't handle 24:00:00 as synonym for midnight (00:00:00) tomorrow
 * + Accepts special values "NaT" (not a time), "Today", (current
 *   day according to local time) and "Now" (current time in UTC).
 *
 * 'str' must be a NULL-terminated string, and 'len' must be its length.
 * 'unit' should contain -1 if the unit is unknown, or the unit
 *      which will be used if it is.
 * 'casting' controls how the detected unit from the string is allowed
 *           to be cast to the 'unit' parameter.
 *
 * 'out' gets filled with the parsed date-time.
 * 'out_local' gets set to 1 if the parsed time was in local time,
 *      to 0 otherwise. The values 'now' and 'today' don't get counted
 *      as local, and neither do UTC +/-#### timezone offsets, because
 *      they aren't using the computer's local timezone offset.
 * 'out_bestunit' gives a suggested unit based on the amount of
 *      resolution provided in the string, or -1 for NaT.
 * 'out_special' gets set to 1 if the parsed time was 'today',
 *      'now', or ''/'NaT'. For 'today', the unit recommended is
 *      'D', for 'now', the unit recommended is 's', and for 'NaT'
 *      the unit recommended is 'Y'.
 *
 * Returns 0 on success, -1 on failure.
 */
NPY_NO_EXPORT int
parse_iso_8601_datetime(char *str, int len,
                    NPY_DATETIMEUNIT unit,
                    NPY_CASTING casting,
                    npy_datetimestruct *out,
                    npy_bool *out_local,
                    NPY_DATETIMEUNIT *out_bestunit,
                    npy_bool *out_special)
{
    int year_leap = 0;
    int i, numdigits;
    char *substr, sublen;
    NPY_DATETIMEUNIT bestunit;

    /* Initialize the output to all zeros */
    memset(out, 0, sizeof(npy_datetimestruct));
    out->month = 1;
    out->day = 1;
    
    /* The empty string and case-variants of "NaT" parse to not-a-time */
    if (len <= 0 || (len == 3 &&
                        tolower(str[0]) == 'n' &&
                        tolower(str[1]) == 'a' &&
                        tolower(str[2]) == 't')) {
        out->year = NPY_DATETIME_NAT;

        /*
         * Indicate that this was a special value, and
         * recommend generic units.
         */
        if (out_local != NULL) {
            *out_local = 0;
        }
        if (out_bestunit != NULL) {
            *out_bestunit = NPY_FR_GENERIC;
        }
        if (out_special != NULL) {
            *out_special = 1;
        }

        return 0;
    }

    if (unit == NPY_FR_GENERIC) {
        PyErr_SetString(PyExc_ValueError,
                    "Cannot create a NumPy datetime other than NaT "
                    "with generic units");
        return -1;
    }

    /*
     * The string "today" resolves to midnight of today's local date in UTC.
     * This is perhaps a little weird, but done so that further truncation
     * to a 'datetime64[D]' type produces the date you expect, rather than
     * switching to an adjacent day depending on the current time and your
     * timezone.
     */
    if (len == 5 && tolower(str[0]) == 't' &&
                    tolower(str[1]) == 'o' &&
                    tolower(str[2]) == 'd' &&
                    tolower(str[3]) == 'a' &&
                    tolower(str[4]) == 'y') {
        time_t rawtime = 0;
        struct tm tm_;

        /* 'today' only works for units of days or larger */
        if (unit != -1 && unit > NPY_FR_D) {
            PyErr_SetString(PyExc_ValueError,
                        "Special value 'today' can only be converted "
                        "to a NumPy datetime with 'D' or larger units");
            return -1;
        }

        time(&rawtime);
#if defined(_WIN32)
        if (localtime_s(&tm_, &rawtime) != 0) {
            PyErr_SetString(PyExc_OSError, "Failed to use localtime_s to "
                                        "get local time");
            return -1;
        }
#else
        /* Other platforms may require something else */
        if (localtime_r(&rawtime, &tm_) == NULL) {
            PyErr_SetString(PyExc_OSError, "Failed to use localtime_r to "
                                        "get local time");
            return -1;
        }
#endif
        out->year = tm_.tm_year + 1900;
        out->month = tm_.tm_mon + 1;
        out->day = tm_.tm_mday;

        bestunit = NPY_FR_D;

        /*
         * Indicate that this was a special value, and
         * is a date (unit 'D').
         */
        if (out_local != NULL) {
            *out_local = 0;
        }
        if (out_bestunit != NULL) {
            *out_bestunit = bestunit;
        }
        if (out_special != NULL) {
            *out_special = 1;
        }

        /* Check the casting rule */
        if (unit != -1 && !can_cast_datetime64_units(bestunit, unit,
                                                     casting)) {
            PyErr_Format(PyExc_ValueError, "Cannot parse \"%s\" as unit "
                         "'%s' using casting rule %s",
                         str, _datetime_strings[unit],
                         npy_casting_to_string(casting));
            return -1;
        }

        return 0;
    }

    /* The string "now" resolves to the current UTC time */
    if (len == 3 && tolower(str[0]) == 'n' &&
                    tolower(str[1]) == 'o' &&
                    tolower(str[2]) == 'w') {
        time_t rawtime = 0;
        PyArray_DatetimeMetaData meta;

        time(&rawtime);

        /* Set up a dummy metadata for the conversion */
        meta.base = NPY_FR_s;
        meta.num = 1;
        meta.events = 1;

        bestunit = NPY_FR_s;

        /*
         * Indicate that this was a special value, and
         * use 's' because the time() function has resolution
         * seconds.
         */
        if (out_local != NULL) {
            *out_local = 0;
        }
        if (out_bestunit != NULL) {
            *out_bestunit = bestunit;
        }
        if (out_special != NULL) {
            *out_special = 1;
        }

        /* Check the casting rule */
        if (unit != -1 && !can_cast_datetime64_units(bestunit, unit,
                                                     casting)) {
            PyErr_Format(PyExc_ValueError, "Cannot parse \"%s\" as unit "
                         "'%s' using casting rule %s",
                         str, _datetime_strings[unit],
                         npy_casting_to_string(casting));
            return -1;
        }

        return convert_datetime_to_datetimestruct(&meta, rawtime, out);
    }

    /* Anything else isn't a special value */
    if (out_special != NULL) {
        *out_special = 0;
    }

    substr = str;
    sublen = len;

    /* Skip leading whitespace */
    while (sublen > 0 && isspace(*substr)) {
        ++substr;
        --sublen;
    }

    /* Leading '-' sign for negative year */
    if (*substr == '-') {
        ++substr;
        --sublen;
    }

    if (sublen == 0) {
        goto parse_error;
    }

    /* PARSE THE YEAR (digits until the '-' character) */
    out->year = 0;
    while (sublen > 0 && isdigit(*substr)) {
        out->year = 10 * out->year + (*substr - '0');
        ++substr;
        --sublen;
    }

    /* Negate the year if necessary */
    if (str[0] == '-') {
        out->year = -out->year;
    }
    /* Check whether it's a leap-year */
    year_leap = is_leapyear(out->year);

    /* Next character must be a '-' or the end of the string */
    if (sublen == 0) {
        if (out_local != NULL) {
            *out_local = 0;
        }
        bestunit = NPY_FR_Y;
        goto finish;
    }
    else if (*substr == '-') {
        ++substr;
        --sublen;
    }
    else {
        goto parse_error;
    }

    /* Can't have a trailing '-' */
    if (sublen == 0) {
        goto parse_error;
    }

    /* PARSE THE MONTH (2 digits) */
    if (sublen >= 2 && isdigit(substr[0]) && isdigit(substr[1])) {
        out->month = 10 * (substr[0] - '0') + (substr[1] - '0');

        if (out->month < 1 || out->month > 12) {
            PyErr_Format(PyExc_ValueError,
                        "Month out of range in datetime string \"%s\"", str);
            goto error;
        }
        substr += 2;
        sublen -= 2;
    }
    else {
        goto parse_error;
    }

    /* Next character must be a '-' or the end of the string */
    if (sublen == 0) {
        if (out_local != NULL) {
            *out_local = 0;
        }
        bestunit = NPY_FR_M;
        goto finish;
    }
    else if (*substr == '-') {
        ++substr;
        --sublen;
    }
    else {
        goto parse_error;
    }

    /* Can't have a trailing '-' */
    if (sublen == 0) {
        goto parse_error;
    }

    /* PARSE THE DAY (2 digits) */
    if (sublen >= 2 && isdigit(substr[0]) && isdigit(substr[1])) {
        out->day = 10 * (substr[0] - '0') + (substr[1] - '0');

        if (out->day < 1 ||
                    out->day > _days_per_month_table[year_leap][out->month-1]) {
            PyErr_Format(PyExc_ValueError,
                        "Day out of range in datetime string \"%s\"", str);
            goto error;
        }
        substr += 2;
        sublen -= 2;
    }
    else {
        goto parse_error;
    }

    /* Next character must be a 'T', ' ', or end of string */
    if (sublen == 0) {
        if (out_local != NULL) {
            *out_local = 0;
        }
        bestunit = NPY_FR_D;
        goto finish;
    }
    else if (*substr != 'T' && *substr != ' ') {
        goto parse_error;
    }
    else {
        ++substr;
        --sublen;
    }

    /* PARSE THE HOURS (2 digits) */
    if (sublen >= 2 && isdigit(substr[0]) && isdigit(substr[1])) {
        out->hour = 10 * (substr[0] - '0') + (substr[1] - '0');

        if (out->hour < 0 || out->hour >= 24) {
            PyErr_Format(PyExc_ValueError,
                        "Hours out of range in datetime string \"%s\"", str);
            goto error;
        }
        substr += 2;
        sublen -= 2;
    }
    else {
        goto parse_error;
    }

    /* Next character must be a ':' or the end of the string */
    if (sublen > 0 && *substr == ':') {
        ++substr;
        --sublen;
    }
    else {
        bestunit = NPY_FR_h;
        goto parse_timezone;
    }

    /* Can't have a trailing ':' */
    if (sublen == 0) {
        goto parse_error;
    }

    /* PARSE THE MINUTES (2 digits) */
    if (sublen >= 2 && isdigit(substr[0]) && isdigit(substr[1])) {
        out->min = 10 * (substr[0] - '0') + (substr[1] - '0');

        if (out->hour < 0 || out->min >= 60) {
            PyErr_Format(PyExc_ValueError,
                        "Minutes out of range in datetime string \"%s\"", str);
            goto error;
        }
        substr += 2;
        sublen -= 2;
    }
    else {
        goto parse_error;
    }

    /* Next character must be a ':' or the end of the string */
    if (sublen > 0 && *substr == ':') {
        ++substr;
        --sublen;
    }
    else {
        bestunit = NPY_FR_m;
        goto parse_timezone;
    }

    /* Can't have a trailing ':' */
    if (sublen == 0) {
        goto parse_error;
    }

    /* PARSE THE SECONDS (2 digits) */
    if (sublen >= 2 && isdigit(substr[0]) && isdigit(substr[1])) {
        out->sec = 10 * (substr[0] - '0') + (substr[1] - '0');

        if (out->sec < 0 || out->sec >= 60) {
            PyErr_Format(PyExc_ValueError,
                        "Seconds out of range in datetime string \"%s\"", str);
            goto error;
        }
        substr += 2;
        sublen -= 2;
    }
    else {
        goto parse_error;
    }

    /* Next character may be a '.' indicating fractional seconds */
    if (sublen > 0 && *substr == '.') {
        ++substr;
        --sublen;
    }
    else {
        bestunit = NPY_FR_s;
        goto parse_timezone;
    }

    /* PARSE THE MICROSECONDS (0 to 6 digits) */
    numdigits = 0;
    for (i = 0; i < 6; ++i) {
        out->us *= 10;
        if (sublen > 0  && isdigit(*substr)) {
            out->us += (*substr - '0');
            ++substr;
            --sublen;
            ++numdigits;
        }
    }

    if (sublen == 0 || !isdigit(*substr)) {
        if (numdigits > 3) {
            bestunit = NPY_FR_us;
        }
        else {
            bestunit = NPY_FR_ms;
        }
        goto parse_timezone;
    }

    /* PARSE THE PICOSECONDS (0 to 6 digits) */
    numdigits = 0;
    for (i = 0; i < 6; ++i) {
        out->ps *= 10;
        if (sublen > 0 && isdigit(*substr)) {
            out->ps += (*substr - '0');
            ++substr;
            --sublen;
            ++numdigits;
        }
    }

    if (sublen == 0 || !isdigit(*substr)) {
        if (numdigits > 3) {
            bestunit = NPY_FR_ps;
        }
        else {
            bestunit = NPY_FR_ns;
        }
        goto parse_timezone;
    }

    /* PARSE THE ATTOSECONDS (0 to 6 digits) */
    numdigits = 0;
    for (i = 0; i < 6; ++i) {
        out->as *= 10;
        if (sublen > 0 && isdigit(*substr)) {
            out->as += (*substr - '0');
            ++substr;
            --sublen;
            ++numdigits;
        }
    }

    if (numdigits > 3) {
        bestunit = NPY_FR_as;
    }
    else {
        bestunit = NPY_FR_fs;
    }

parse_timezone:
    if (sublen == 0) {
        /*
         * ISO 8601 states to treat date-times without a timezone offset
         * or 'Z' for UTC as local time. The C standard libary functions
         * mktime and gmtime allow us to do this conversion.
         *
         * Only do this timezone adjustment for recent and future years.
         */
        if (out->year > 1900 && out->year < 10000) {
            time_t rawtime = 0;
            struct tm tm_;

            tm_.tm_sec = out->sec;
            tm_.tm_min = out->min;
            tm_.tm_hour = out->hour;
            tm_.tm_mday = out->day;
            tm_.tm_mon = out->month - 1;
            tm_.tm_year = out->year - 1900;
            tm_.tm_isdst = -1;

            /* mktime converts a local 'struct tm' into a time_t */
            rawtime = mktime(&tm_);
            if (rawtime == -1) {
                PyErr_SetString(PyExc_OSError, "Failed to use mktime to "
                                            "convert local time to UTC");
                goto error;
            }

            /* gmtime converts a 'time_t' into a UTC 'struct tm' */
#if defined(_WIN32)
            if (gmtime_s(&tm_, &rawtime) != 0) {
                PyErr_SetString(PyExc_OSError, "Failed to use gmtime_s to "
                                            "get a UTC time");
                goto error;
            }
#else
            /* Other platforms may require something else */
            if (gmtime_r(&rawtime, &tm_) == NULL) {
                PyErr_SetString(PyExc_OSError, "Failed to use gmtime_r to "
                                            "get a UTC time");
                goto error;
            }
#endif
            out->sec = tm_.tm_sec;
            out->min = tm_.tm_min;
            out->hour = tm_.tm_hour;
            out->day = tm_.tm_mday;
            out->month = tm_.tm_mon + 1;
            out->year = tm_.tm_year + 1900;
        }

        /* Since neither "Z" nor a time-zone was specified, it's local */
        if (out_local != NULL) {
            *out_local = 1;
        }

        goto finish;
    }

    /* UTC specifier */
    if (*substr == 'Z') {
        /* "Z" means not local */
        if (out_local != NULL) {
            *out_local = 0;
        }

        if (sublen == 1) {
            goto finish;
        }
        else {
            ++substr;
            --sublen;
        }
    }
    /* Time zone offset */
    else if (*substr == '-' || *substr == '+') {
        int offset_neg = 0, offset_hour = 0, offset_minute = 0;

        /*
         * Since "local" means local with respect to the current
         * machine, we say this is non-local.
         */
        if (out_local != NULL) {
            *out_local = 0;
        }

        if (*substr == '-') {
            offset_neg = 1;
        }
        ++substr;
        --sublen;

        /* The hours offset */
        if (sublen >= 2 && isdigit(substr[0]) && isdigit(substr[1])) {
            offset_hour = 10 * (substr[0] - '0') + (substr[1] - '0');
            substr += 2;
            sublen -= 2;
            if (offset_hour >= 24) {
                PyErr_Format(PyExc_ValueError,
                            "Timezone hours offset out of range "
                            "in datetime string \"%s\"", str);
                goto error;
            }
        }
        else {
            goto parse_error;
        }

        /* The minutes offset is optional */
        if (sublen > 0) {
            /* Optional ':' */
            if (*substr == ':') {
                ++substr;
                --sublen;
            }

            /* The minutes offset (at the end of the string) */
            if (sublen >= 2 && isdigit(substr[0]) && isdigit(substr[1])) {
                offset_minute = 10 * (substr[0] - '0') + (substr[1] - '0');
                substr += 2;
                sublen -= 2;
                if (offset_minute >= 60) {
                    PyErr_Format(PyExc_ValueError,
                                "Timezone minutes offset out of range "
                                "in datetime string \"%s\"", str);
                    goto error;
                }
            }
            else {
                goto parse_error;
            }
        }

        /* Apply the time zone offset */
        if (offset_neg) {
            offset_hour = -offset_hour;
            offset_minute = -offset_minute;
        }
        add_minutes_to_datetimestruct(out, -60 * offset_hour - offset_minute);
    }

    /* Skip trailing whitespace */
    while (sublen > 0 && isspace(*substr)) {
        ++substr;
        --sublen;
    }

    if (sublen != 0) {
        goto parse_error;
    }

finish:
    if (out_bestunit != NULL) {
        *out_bestunit = bestunit;
    }

    /* Check the casting rule */
    if (unit != -1 && !can_cast_datetime64_units(bestunit, unit,
                                                 casting)) {
        PyErr_Format(PyExc_ValueError, "Cannot parse \"%s\" as unit "
                     "'%s' using casting rule %s",
                     str, _datetime_strings[unit],
                     npy_casting_to_string(casting));
        return -1;
    }

    return 0;

parse_error:
    PyErr_Format(PyExc_ValueError,
            "Error parsing datetime string \"%s\" at position %d",
            str, (int)(substr-str));
    return -1;

error:
    return -1;
}

/*
 * Provides a string length to use for converting datetime
 * objects with the given local and unit settings.
 */
NPY_NO_EXPORT int
get_datetime_iso_8601_strlen(int local, NPY_DATETIMEUNIT base)
{
    int len = 0;

    /* If no unit is provided, return the maximum length */
    if (base == -1) {
        return NPY_DATETIME_MAX_ISO8601_STRLEN;
    }

    switch (base) {
        /* Generic units can only be used to represent NaT */
        case NPY_FR_GENERIC:
            return 4;
        case NPY_FR_as:
            len += 3;  /* "###" */
        case NPY_FR_fs:
            len += 3;  /* "###" */
        case NPY_FR_ps:
            len += 3;  /* "###" */
        case NPY_FR_ns:
            len += 3;  /* "###" */
        case NPY_FR_us:
            len += 3;  /* "###" */
        case NPY_FR_ms:
            len += 4;  /* ".###" */
        case NPY_FR_s:
            len += 3;  /* ":##" */
        case NPY_FR_m:
            len += 3;  /* ":##" */
        case NPY_FR_h:
            len += 3;  /* "T##" */
        case NPY_FR_D:
        case NPY_FR_W:
            len += 3;  /* "-##" */
        case NPY_FR_M:
            len += 3;  /* "-##" */
        case NPY_FR_Y:
            len += 21; /* 64-bit year */
            break;
    }

    if (base >= NPY_FR_h) {
        if (local) {
            len += 5;  /* "+####" or "-####" */
        }
        else {
            len += 1;  /* "Z" */
        }
    }

    len += 1; /* NULL terminator */

    return len;
}

/*
 * Converts an npy_datetimestruct to an (almost) ISO 8601
 * NULL-terminated string.
 *
 * If 'local' is non-zero, it produces a string in local time with
 * a +-#### timezone offset, otherwise it uses timezone Z (UTC).
 *
 * 'base' restricts the output to that unit. Set 'base' to
 * -1 to auto-detect a base after which all the values are zero.
 *
 *  'tzoffset' is used if 'local' is enabled, and 'tzoffset' is
 *  set to a value other than -1. This is a manual override for
 *  the local time zone to use, as an offset in minutes.
 *
 *  Returns 0 on success, -1 on failure (for example if the output
 *  string was too short).
 */
NPY_NO_EXPORT int
make_iso_8601_datetime(npy_datetimestruct *dts, char *outstr, int outlen,
                    int local, NPY_DATETIMEUNIT base, int tzoffset)
{
    npy_datetimestruct dts_local;
    int timezone_offset = 0;

    char *substr = outstr, sublen = outlen;
    int tmplen;

    /* Handle NaT, and treat a datetime with generic units as NaT */
    if (dts->year == NPY_DATETIME_NAT || base == NPY_FR_GENERIC) {
        if (outlen < 4) {
            goto string_too_short;
        }
        outstr[0] = 'N';
        outstr[0] = 'a';
        outstr[0] = 'T';
        outstr[0] = '\0';

        return 0;
    }

    /* Only do local time within a reasonable year range */
    if ((dts->year <= 1900 || dts->year >= 10000) && tzoffset == -1) {
        local = 0;
    }

    /* Automatically detect a good unit */
    if (base == -1) {
        if (dts->as % 1000 != 0) {
            base = NPY_FR_as;
        }
        else if (dts->as != 0) {
            base = NPY_FR_fs;
        }
        else if (dts->ps % 1000 != 0) {
            base = NPY_FR_ps;
        }
        else if (dts->ps != 0) {
            base = NPY_FR_ns;
        }
        else if (dts->us % 1000 != 0) {
            base = NPY_FR_us;
        }
        else if (dts->us != 0) {
            base = NPY_FR_ms;
        }
        else if (dts->sec != 0) {
            base = NPY_FR_s;
        }
        /*
         * hours and minutes don't get split up by default, and printing
         * in local time forces minutes
         */
        else if (local || dts->min != 0 || dts->hour != 0) {
            base = NPY_FR_m;
        }
        /* dates don't get split up by default */
        else {
            base = NPY_FR_D;
        }
    }
    /*
     * Print weeks with the same precision as days.
     *
     * TODO: Could print weeks with YYYY-Www format if the week
     *       epoch is a Monday.
     */
    else if (base == NPY_FR_W) {
        base = NPY_FR_D;
    }

    /* Printed dates have no time zone */
    if (base < NPY_FR_h) {
        local = 0;
    }

    /* Use the C API to convert from UTC to local time */
    if (local && tzoffset == -1) {
        time_t rawtime = 0, localrawtime;
        struct tm tm_;

        /*
         * Convert everything in 'dts' to a time_t, to minutes precision.
         * This is POSIX time, which skips leap-seconds, but because
         * we drop the seconds value from the npy_datetimestruct, everything
         * is ok for this operation.
         */
        rawtime = (time_t)get_datetimestruct_days(dts) * 24 * 60 * 60;
        rawtime += dts->hour * 60 * 60;
        rawtime += dts->min * 60;

        /* localtime converts a 'time_t' into a local 'struct tm' */
#if defined(_WIN32)
        if (localtime_s(&tm_, &rawtime) != 0) {
            PyErr_SetString(PyExc_OSError, "Failed to use localtime_s to "
                                        "get a local time");
            return -1;
        }
#else
        /* Other platforms may require something else */
        if (localtime_r(&rawtime, &tm_) == NULL) {
            PyErr_SetString(PyExc_OSError, "Failed to use localtime_r to "
                                        "get a local time");
            return -1;
        }
#endif
        /* Make a copy of the npy_datetimestruct we can modify */
        dts_local = *dts;

        /* Copy back all the values except seconds */
        dts_local.min = tm_.tm_min;
        dts_local.hour = tm_.tm_hour;
        dts_local.day = tm_.tm_mday;
        dts_local.month = tm_.tm_mon + 1;
        dts_local.year = tm_.tm_year + 1900;

        /* Extract the timezone offset that was applied */
        rawtime /= 60;
        localrawtime = (time_t)get_datetimestruct_days(&dts_local) * 24 * 60;
        localrawtime += dts_local.hour * 60;
        localrawtime += dts_local.min;

        timezone_offset = localrawtime - rawtime;

        /* Set dts to point to our local time instead of the UTC time */
        dts = &dts_local;
    }
    /* Use the manually provided tzoffset */
    else if (local) {
        /* Make a copy of the npy_datetimestruct we can modify */
        dts_local = *dts;
        dts = &dts_local;

        /* Set and apply the required timezone offset */
        timezone_offset = tzoffset;
        add_minutes_to_datetimestruct(dts, timezone_offset);
    }

    /* YEAR */
#ifdef _WIN32
    tmplen = _snprintf(substr, sublen, "%04" NPY_INT64_FMT, dts->year);
#else
    tmplen = snprintf(substr, sublen, "%04" NPY_INT64_FMT, dts->year);
#endif
    /* If it ran out of space or there isn't space for the NULL terminator */
    if (tmplen < 0 || tmplen >= sublen) {
        goto string_too_short;
    }
    substr += tmplen;
    sublen -= tmplen;

    /* Stop if the unit is years */
    if (base == NPY_FR_Y) {
        *substr = '\0';
        return 0;
    }

    /* MONTH */
    substr[0] = '-';
    if (sublen <= 1 ) {
        goto string_too_short;
    }
    substr[1] = (char)((dts->month / 10) + '0');
    if (sublen <= 2 ) {
        goto string_too_short;
    }
    substr[2] = (char)((dts->month % 10) + '0');
    if (sublen <= 3 ) {
        goto string_too_short;
    }
    substr += 3;
    sublen -= 3;

    /* Stop if the unit is months */
    if (base == NPY_FR_M) {
        *substr = '\0';
        return 0;
    }

    /* DAY */
    substr[0] = '-';
    if (sublen <= 1 ) {
        goto string_too_short;
    }
    substr[1] = (char)((dts->day / 10) + '0');
    if (sublen <= 2 ) {
        goto string_too_short;
    }
    substr[2] = (char)((dts->day % 10) + '0');
    if (sublen <= 3 ) {
        goto string_too_short;
    }
    substr += 3;
    sublen -= 3;

    /* Stop if the unit is days */
    if (base == NPY_FR_D) {
        *substr = '\0';
        return 0;
    }

    /* HOUR */
    substr[0] = 'T';
    if (sublen <= 1 ) {
        goto string_too_short;
    }
    substr[1] = (char)((dts->hour / 10) + '0');
    if (sublen <= 2 ) {
        goto string_too_short;
    }
    substr[2] = (char)((dts->hour % 10) + '0');
    if (sublen <= 3 ) {
        goto string_too_short;
    }
    substr += 3;
    sublen -= 3;

    /* Stop if the unit is hours */
    if (base == NPY_FR_h) {
        goto add_time_zone;
    }

    /* MINUTE */
    substr[0] = ':';
    if (sublen <= 1 ) {
        goto string_too_short;
    }
    substr[1] = (char)((dts->min / 10) + '0');
    if (sublen <= 2 ) {
        goto string_too_short;
    }
    substr[2] = (char)((dts->min % 10) + '0');
    if (sublen <= 3 ) {
        goto string_too_short;
    }
    substr += 3;
    sublen -= 3;

    /* Stop if the unit is minutes */
    if (base == NPY_FR_m) {
        goto add_time_zone;
    }

    /* SECOND */
    substr[0] = ':';
    if (sublen <= 1 ) {
        goto string_too_short;
    }
    substr[1] = (char)((dts->sec / 10) + '0');
    if (sublen <= 2 ) {
        goto string_too_short;
    }
    substr[2] = (char)((dts->sec % 10) + '0');
    if (sublen <= 3 ) {
        goto string_too_short;
    }
    substr += 3;
    sublen -= 3;

    /* Stop if the unit is seconds */
    if (base == NPY_FR_s) {
        goto add_time_zone;
    }

    /* MILLISECOND */
    substr[0] = '.';
    if (sublen <= 1 ) {
        goto string_too_short;
    }
    substr[1] = (char)((dts->us / 100000) % 10 + '0');
    if (sublen <= 2 ) {
        goto string_too_short;
    }
    substr[2] = (char)((dts->us / 10000) % 10 + '0');
    if (sublen <= 3 ) {
        goto string_too_short;
    }
    substr[3] = (char)((dts->us / 1000) % 10 + '0');
    if (sublen <= 4 ) {
        goto string_too_short;
    }
    substr += 4;
    sublen -= 4;

    /* Stop if the unit is milliseconds */
    if (base == NPY_FR_ms) {
        goto add_time_zone;
    }

    /* MICROSECOND */
    substr[0] = (char)((dts->us / 100) % 10 + '0');
    if (sublen <= 1 ) {
        goto string_too_short;
    }
    substr[1] = (char)((dts->us / 10) % 10 + '0');
    if (sublen <= 2 ) {
        goto string_too_short;
    }
    substr[2] = (char)(dts->us % 10 + '0');
    if (sublen <= 3 ) {
        goto string_too_short;
    }
    substr += 3;
    sublen -= 3;

    /* Stop if the unit is microseconds */
    if (base == NPY_FR_us) {
        goto add_time_zone;
    }

    /* NANOSECOND */
    substr[0] = (char)((dts->ps / 100000) % 10 + '0');
    if (sublen <= 1 ) {
        goto string_too_short;
    }
    substr[1] = (char)((dts->ps / 10000) % 10 + '0');
    if (sublen <= 2 ) {
        goto string_too_short;
    }
    substr[2] = (char)((dts->ps / 1000) % 10 + '0');
    if (sublen <= 3 ) {
        goto string_too_short;
    }
    substr += 3;
    sublen -= 3;

    /* Stop if the unit is nanoseconds */
    if (base == NPY_FR_ns) {
        goto add_time_zone;
    }

    /* PICOSECOND */
    substr[0] = (char)((dts->ps / 100) % 10 + '0');
    if (sublen <= 1 ) {
        goto string_too_short;
    }
    substr[1] = (char)((dts->ps / 10) % 10 + '0');
    if (sublen <= 2 ) {
        goto string_too_short;
    }
    substr[2] = (char)(dts->ps % 10 + '0');
    if (sublen <= 3 ) {
        goto string_too_short;
    }
    substr += 3;
    sublen -= 3;

    /* Stop if the unit is picoseconds */
    if (base == NPY_FR_ps) {
        goto add_time_zone;
    }

    /* FEMTOSECOND */
    substr[0] = (char)((dts->as / 100000) % 10 + '0');
    if (sublen <= 1 ) {
        goto string_too_short;
    }
    substr[1] = (char)((dts->as / 10000) % 10 + '0');
    if (sublen <= 2 ) {
        goto string_too_short;
    }
    substr[2] = (char)((dts->as / 1000) % 10 + '0');
    if (sublen <= 3 ) {
        goto string_too_short;
    }
    substr += 3;
    sublen -= 3;

    /* Stop if the unit is femtoseconds */
    if (base == NPY_FR_fs) {
        goto add_time_zone;
    }

    /* ATTOSECOND */
    substr[0] = (char)((dts->as / 100) % 10 + '0');
    if (sublen <= 1 ) {
        goto string_too_short;
    }
    substr[1] = (char)((dts->as / 10) % 10 + '0');
    if (sublen <= 2 ) {
        goto string_too_short;
    }
    substr[2] = (char)(dts->as % 10 + '0');
    if (sublen <= 3 ) {
        goto string_too_short;
    }
    substr += 3;
    sublen -= 3;

add_time_zone:
    if (local) {
        /* Add the +/- sign */
        if (timezone_offset < 0) {
            substr[0] = '-';
            timezone_offset = -timezone_offset;
        }
        else {
            substr[0] = '+';
        }
        if (sublen <= 1) {
            goto string_too_short;
        }
        substr += 1;
        sublen -= 1;

        /* Add the timezone offset */
        substr[0] = (char)((timezone_offset / (10*60)) % 10 + '0');
        if (sublen <= 1 ) {
            goto string_too_short;
        }
        substr[1] = (char)((timezone_offset / 60) % 10 + '0');
        if (sublen <= 2 ) {
            goto string_too_short;
        }
        substr[2] = (char)(((timezone_offset % 60) / 10) % 10 + '0');
        if (sublen <= 3 ) {
            goto string_too_short;
        }
        substr[3] = (char)((timezone_offset % 60) % 10 + '0');
        if (sublen <= 4 ) {
            goto string_too_short;
        }
        substr += 4;
        sublen -= 4;
    }
    /* UTC "Zulu" time */
    else {
        substr[0] = 'Z';
        if (sublen <= 1) {
            goto string_too_short;
        }
        substr += 1;
        sublen -= 1;
    }

    /* Add a NULL terminator, and return */
    substr[0] = '\0';

    return 0;

string_too_short:
    /* Put a NULL terminator on anyway */
    if (outlen > 0) {
        outstr[outlen-1] = '\0';
    }

    PyErr_Format(PyExc_RuntimeError,
                "The string provided for NumPy ISO datetime formatting "
                "was too short, with length %d",
                outlen);
    return -1;
}


