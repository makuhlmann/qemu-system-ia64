/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * EFI Unicode Collation protocol (ISO 8859-1 case folding, FAT 8.3
 * conversion).
 */

#include "fw-base.h"
#include "fw-efi-types.h"
#include "fw-services.h"

static CHAR16 unicode_to_upper(CHAR16 ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return (CHAR16)(ch - ('a' - 'A'));
    }
    if ((ch >= 0x00e0 && ch <= 0x00f6) ||
        (ch >= 0x00f8 && ch <= 0x00fe)) {
        return (CHAR16)(ch - 0x20);
    }
    return ch;
}

static CHAR16 unicode_to_lower(CHAR16 ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (CHAR16)(ch + ('a' - 'A'));
    }
    if ((ch >= 0x00c0 && ch <= 0x00d6) ||
        (ch >= 0x00d8 && ch <= 0x00de)) {
        return (CHAR16)(ch + 0x20);
    }
    return ch;
}

static INTN unicode_stricoll(EFI_UNICODE_COLLATION_PROTOCOL *This,
                             CHAR16 *String1, CHAR16 *String2)
{
    (void)This;
    while (*String1 != 0 && *String2 != 0) {
        CHAR16 c1 = unicode_to_upper(*String1);
        CHAR16 c2 = unicode_to_upper(*String2);
        if (c1 != c2) {
            return (INTN)c1 - (INTN)c2;
        }
        String1++;
        String2++;
    }
    return (INTN)unicode_to_upper(*String1) - (INTN)unicode_to_upper(*String2);
}

static BOOLEAN unicode_metai_set_matches(CHAR16 Character, CHAR16 **Pattern,
                                         BOOLEAN *Valid)
{
    CHAR16 *p = *Pattern;
    CHAR16 folded = unicode_to_upper(Character);
    BOOLEAN have_element = 0;
    BOOLEAN matched = 0;

    while (*p != 0 && *p != ']') {
        CHAR16 low = unicode_to_upper(*p++);

        have_element = 1;
        if (*p == '-' && p[1] != 0 && p[1] != ']') {
            CHAR16 high;

            p++;
            high = unicode_to_upper(*p++);
            if (folded >= low && folded <= high) {
                matched = 1;
            }
        } else if (folded == low) {
            matched = 1;
        }
    }

    if (*p != ']' || !have_element) {
        *Valid = 0;
        return 0;
    }
    *Pattern = p + 1;
    *Valid = 1;
    return matched;
}

static BOOLEAN unicode_metai_match(EFI_UNICODE_COLLATION_PROTOCOL *This,
                                   CHAR16 *String, CHAR16 *Pattern)
{
    CHAR16 *star_pattern = NULL;
    CHAR16 *star_string = NULL;

    (void)This;
    for (;;) {
        BOOLEAN matched = 0;

        if (*Pattern == '*') {
            do {
                Pattern++;
            } while (*Pattern == '*');
            if (*Pattern == 0) {
                return 1;
            }
            star_pattern = Pattern;
            star_string = String;
            continue;
        }
        if (*Pattern == 0) {
            if (*String == 0) {
                return 1;
            }
        } else if (*String != 0) {
            if (*Pattern == '?') {
                Pattern++;
                matched = 1;
            } else if (*Pattern == '[') {
                BOOLEAN valid;

                Pattern++;
                matched = unicode_metai_set_matches(*String, &Pattern,
                                                     &valid);
                if (!valid) {
                    return 0;
                }
            } else {
                matched = unicode_to_upper(*Pattern) ==
                          unicode_to_upper(*String);
                Pattern++;
            }
        }
        if (matched) {
            String++;
            continue;
        }
        if (star_pattern == NULL || *star_string == 0) {
            return 0;
        }
        star_string++;
        String = star_string;
        Pattern = star_pattern;
    }
}

static VOID unicode_str_lwr(EFI_UNICODE_COLLATION_PROTOCOL *This, CHAR16 *String)
{
    (void)This;
    while (*String != 0) {
        *String = unicode_to_lower(*String);
        String++;
    }
}

static VOID unicode_str_upr(EFI_UNICODE_COLLATION_PROTOCOL *This, CHAR16 *String)
{
    (void)This;
    while (*String != 0) {
        *String = unicode_to_upper(*String);
        String++;
    }
}

static VOID unicode_fat_to_str(EFI_UNICODE_COLLATION_PROTOCOL *This,
                               UINTN FatSize, CHAR8 *Fat, CHAR16 *String)
{
    UINTN i;

    (void)This;
    for (i = 0; i < FatSize && Fat[i] != 0; i++) {
        String[i] = (CHAR16)(UINT8)Fat[i];
    }
    String[i] = 0;
}

static BOOLEAN unicode_fat_char_is_valid(CHAR16 ch)
{
    static const CHAR8 valid_punctuation[] =
        "\\._^$~!#%&-{}()@`'";
    UINTN i;

    if ((ch >= '0' && ch <= '9') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= 'a' && ch <= 'z') ||
        (ch >= 0x00c0 && ch <= 0x00d6) ||
        (ch >= 0x00d8 && ch <= 0x00f6) ||
        (ch >= 0x00f8 && ch <= 0x00fe)) {
        return 1;
    }
    for (i = 0; valid_punctuation[i] != 0; i++) {
        if (ch == (CHAR16)(UINT8)valid_punctuation[i]) {
            return 1;
        }
    }
    return 0;
}

static BOOLEAN unicode_str_to_fat(EFI_UNICODE_COLLATION_PROTOCOL *This,
                                  CHAR16 *String, UINTN FatSize, CHAR8 *Fat)
{
    BOOLEAN long_name = 0;

    (void)This;
    while (*String != 0 && FatSize != 0) {
        CHAR16 ch = *String++;

        if (ch == '.' || ch == ' ') {
            continue;
        }
        if (ch <= 0x00ff && unicode_fat_char_is_valid(ch)) {
            *Fat = (CHAR8)(UINT8)unicode_to_upper(ch);
        } else {
            *Fat = '_';
            long_name = 1;
        }
        Fat++;
        FatSize--;
    }
    return long_name;
}

EFI_UNICODE_COLLATION_PROTOCOL mUnicodeCollationProto = {
    .StriColl = unicode_stricoll,
    .MetaiMatch = unicode_metai_match,
    .StrLwr = unicode_str_lwr,
    .StrUpr = unicode_str_upr,
    .FatToStr = unicode_fat_to_str,
    .StrToFat = unicode_str_to_fat,
    .SupportedLanguages = "eng",
};

BOOLEAN unicode_collation_selftest(void)
{
    CHAR16 mixed_case[] = { 'B', 'o', 'O', 't', 0 };
    CHAR16 lower_case[] = { 'b', 'O', 'o', 'T', 0 };
    CHAR16 wildcard_name[] = {
        'B', 'O', 'O', 'T', 'I', 'A', '6', '4', '.', 'E', 'F', 'I', 0
    };
    CHAR16 wildcard_pattern[] = { '*', '.', 'e', 'f', 'i', 0 };
    CHAR16 range_name[] = { 'D', '7', '.', 'f', 'w', 0 };
    CHAR16 range_pattern[] = {
        '[', 'a', '-', 'z', ']', '?', '.', '[', 'F', 'W', ']', 'w', 0
    };
    CHAR16 symbol_name[] = { '!', 0 };
    CHAR16 symbol_pattern[] = {
        '[', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', ']', 0
    };
    CHAR16 malformed_pattern[] = { '[', 'a', '-', 'z', 0 };
    CHAR16 empty_string[] = { 0 };
    CHAR16 repeated_star[] = { '*', '*', 0 };
    CHAR16 latin_case[] = { 0x00c4, 0x00f6, 0 };
    CHAR8 fat_source[] = { 'A', 'B', 0, 'C' };
    CHAR16 fat_string[5] = { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff };
    CHAR16 short_source[] = {
        'r', 'e', 'a', 'd', ' ', 'm', 'e', '.', 't', 'x', 't', 0
    };
    CHAR16 long_source[] = { 'a', '+', 0x0100, 'b', 0 };
    CHAR8 short_name[12];
    CHAR8 long_name[5];

    if (mUnicodeCollationProto.StriColl(&mUnicodeCollationProto, mixed_case,
                                       lower_case) != 0 ||
        !mUnicodeCollationProto.MetaiMatch(&mUnicodeCollationProto,
                                          wildcard_name,
                                          wildcard_pattern) ||
        !mUnicodeCollationProto.MetaiMatch(&mUnicodeCollationProto,
                                          range_name, range_pattern) ||
        !mUnicodeCollationProto.MetaiMatch(&mUnicodeCollationProto,
                                          symbol_name, symbol_pattern) ||
        mUnicodeCollationProto.MetaiMatch(&mUnicodeCollationProto,
                                         symbol_name, malformed_pattern) ||
        !mUnicodeCollationProto.MetaiMatch(&mUnicodeCollationProto,
                                          empty_string, repeated_star)) {
        return 0;
    }

    mUnicodeCollationProto.StrLwr(&mUnicodeCollationProto, latin_case);
    if (latin_case[0] != 0x00e4 || latin_case[1] != 0x00f6) {
        return 0;
    }
    mUnicodeCollationProto.StrUpr(&mUnicodeCollationProto, latin_case);
    if (latin_case[0] != 0x00c4 || latin_case[1] != 0x00d6) {
        return 0;
    }

    mUnicodeCollationProto.FatToStr(&mUnicodeCollationProto,
                                   sizeof(fat_source), fat_source, fat_string);
    if (fat_string[0] != 'A' || fat_string[1] != 'B' ||
        fat_string[2] != 0 || fat_string[3] != 0xffff) {
        return 0;
    }
    fw_set_mem(short_name, sizeof(short_name), 0x5a);
    if (mUnicodeCollationProto.StrToFat(&mUnicodeCollationProto, short_source,
                                       sizeof(short_name), short_name) ||
        short_name[0] != 'R' || short_name[1] != 'E' ||
        short_name[2] != 'A' || short_name[3] != 'D' ||
        short_name[4] != 'M' || short_name[5] != 'E' ||
        short_name[6] != 'T' || short_name[7] != 'X' ||
        short_name[8] != 'T' ||
        short_name[9] != 0x5a) {
        return 0;
    }
    fw_set_mem(long_name, sizeof(long_name), 0x5a);
    if (!mUnicodeCollationProto.StrToFat(&mUnicodeCollationProto, long_source,
                                        sizeof(long_name), long_name) ||
        long_name[0] != 'A' || long_name[1] != '_' ||
        long_name[2] != '_' || long_name[3] != 'B' ||
        long_name[4] != 0x5a) {
        return 0;
    }
    return 1;
}
