/*
 * xml_windows.c
 *
 * Set Windows-specific metadata in a WIM file's XML document based on the image
 * contents.
 */

/*
 * Copyright 2016-2023 Eric Biggers
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option) any
 * later version.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this file; if not, see https://www.gnu.org/licenses/.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>

#include "wimlib.h"
#include "wimlib/blob_table.h"
#include "wimlib/dentry.h"
#include "wimlib/encoding.h"
#include "wimlib/endianness.h"
#include "wimlib/error.h"
#include "wimlib/metadata.h"
#include "wimlib/registry.h"
#include "wimlib/wim.h"
#include "wimlib/xml_windows.h"

/* Context for a call to set_windows_specific_info()  */
struct windows_info_ctx {
	WIMStruct *wim;
	int image;
	bool oom_encountered;
	bool debug_enabled;
};

/* For debugging purposes, the environmental variable WIMLIB_DEBUG_XML_INFO can
 * be set to enable messages about certain things not being as expected in the
 * registry or other files used as information sources.  */

#define XML_WARN(format, ...) \
  if (ctx->debug_enabled)     \
  WARNING(format, ##__VA_ARGS__)

/* Set a property in the XML document, with error checking.  */
static void
set_string_property(struct windows_info_ctx *ctx,
                    const tchar *name,
                    const tchar *value)
{
	int ret = wimlib_set_image_property(ctx->wim, ctx->image, name, value);
	if (likely(!ret))
		return;

	ctx->oom_encountered |= (ret == WIMLIB_ERR_NOMEM);
	WARNING("Failed to set image property \"%" TS "\" to value "
	        "\"%" TS "\": %" TS,
	        name,
	        value,
	        wimlib_get_error_string(ret));
}

/* Set a property in the XML document, with error checking.  */
static void
set_number_property(struct windows_info_ctx *ctx, const tchar *name, s64 value)
{
	tchar buffer[32];
	tsprintf(buffer, T("%" PRIi64 ""), value);
	set_string_property(ctx, name, buffer);
}

/* Check the result of a registry hive operation.  If unsuccessful, possibly
 * print debugging information.  Return true iff successful.  */
static bool
check_hive_status(struct windows_info_ctx *ctx,
                  enum hive_status status,
                  const tchar *key,
                  const tchar *value)
{
	if (likely(status == HIVE_OK))
		return true;

	ctx->oom_encountered |= (status == HIVE_OUT_OF_MEMORY);
	XML_WARN("%s; key=%" TS " value=%" TS,
	         hive_status_to_string(status),
	         (key ? key : T("(null)")),
	         (value ? value : T("(null)")));
	return false;
}

static bool
is_registry_valid(struct windows_info_ctx *ctx,
                  const void *hive_mem,
                  size_t hive_size)
{
	enum hive_status status;

	status = hive_validate(hive_mem, hive_size);
	return check_hive_status(ctx, status, NULL, NULL);
}

static bool
get_string_from_registry(struct windows_info_ctx *ctx,
                         const struct regf *regf,
                         const tchar *key_name,
                         const tchar *value_name,
                         tchar **value_ret)
{
	enum hive_status status;

	status = hive_get_string(regf, key_name, value_name, value_ret);
	return check_hive_status(ctx, status, key_name, value_name);
}

static bool
get_number_from_registry(struct windows_info_ctx *ctx,
                         const struct regf *regf,
                         const tchar *key_name,
                         const tchar *value_name,
                         s64 *value_ret)
{
	enum hive_status status;

	status = hive_get_number(regf, key_name, value_name, value_ret);
	return check_hive_status(ctx, status, key_name, value_name);
}

static bool
list_subkeys_in_registry(struct windows_info_ctx *ctx,
                         const struct regf *regf,
                         const tchar *key_name,
                         tchar ***subkeys_ret)
{
	enum hive_status status;

	status = hive_list_subkeys(regf, key_name, subkeys_ret);
	return check_hive_status(ctx, status, key_name, NULL);
}

/* Copy a string value from a registry hive to the XML document.  */
static void
copy_registry_string(struct windows_info_ctx *ctx,
                     const struct regf *regf,
                     const tchar *key_name,
                     const tchar *value_name,
                     const tchar *property_name)
{
	tchar *string;

	if (get_string_from_registry(ctx, regf, key_name, value_name, &string))
	{
		set_string_property(ctx, property_name, string);
		FREE(string);
	}
}

/* A table that map Windows language IDs, sorted numerically, to their language
 * names.  It was generated by tools/generate_language_id_map.c.  */
static const struct {
	u16 id;
	u16 name_start_offset;
} language_id_map[453] = {
	{ 0x0000, 0 },    { 0x0001, 6 },    { 0x0002, 12 },   { 0x0003, 18 },
	{ 0x0004, 24 },   { 0x0005, 30 },   { 0x0006, 36 },   { 0x0007, 42 },
	{ 0x0008, 48 },   { 0x0009, 54 },   { 0x000a, 60 },   { 0x000b, 66 },
	{ 0x000c, 72 },   { 0x000d, 78 },   { 0x000e, 84 },   { 0x000f, 90 },
	{ 0x0010, 96 },   { 0x0011, 102 },  { 0x0012, 108 },  { 0x0013, 114 },
	{ 0x0014, 120 },  { 0x0015, 126 },  { 0x0016, 132 },  { 0x0017, 138 },
	{ 0x0018, 144 },  { 0x0019, 150 },  { 0x001a, 156 },  { 0x001b, 162 },
	{ 0x001c, 168 },  { 0x001d, 174 },  { 0x001e, 180 },  { 0x001f, 186 },
	{ 0x0020, 192 },  { 0x0021, 198 },  { 0x0022, 204 },  { 0x0023, 210 },
	{ 0x0024, 216 },  { 0x0025, 222 },  { 0x0026, 228 },  { 0x0027, 234 },
	{ 0x0028, 240 },  { 0x0029, 251 },  { 0x002a, 257 },  { 0x002b, 263 },
	{ 0x002c, 269 },  { 0x002d, 280 },  { 0x002e, 286 },  { 0x002f, 293 },
	{ 0x0030, 299 },  { 0x0031, 305 },  { 0x0032, 311 },  { 0x0033, 317 },
	{ 0x0034, 323 },  { 0x0035, 329 },  { 0x0036, 335 },  { 0x0037, 341 },
	{ 0x0038, 347 },  { 0x0039, 353 },  { 0x003a, 359 },  { 0x003b, 365 },
	{ 0x003c, 371 },  { 0x003d, 377 },  { 0x003e, 384 },  { 0x003f, 390 },
	{ 0x0040, 396 },  { 0x0041, 402 },  { 0x0042, 408 },  { 0x0043, 414 },
	{ 0x0044, 425 },  { 0x0045, 431 },  { 0x0046, 437 },  { 0x0047, 443 },
	{ 0x0048, 449 },  { 0x0049, 455 },  { 0x004a, 461 },  { 0x004b, 467 },
	{ 0x004c, 473 },  { 0x004d, 479 },  { 0x004e, 485 },  { 0x004f, 491 },
	{ 0x0050, 497 },  { 0x0051, 503 },  { 0x0052, 509 },  { 0x0053, 515 },
	{ 0x0054, 521 },  { 0x0055, 527 },  { 0x0056, 533 },  { 0x0057, 539 },
	{ 0x0058, 546 },  { 0x0059, 553 },  { 0x005a, 564 },  { 0x005b, 571 },
	{ 0x005c, 577 },  { 0x005d, 589 },  { 0x005e, 600 },  { 0x005f, 606 },
	{ 0x0060, 618 },  { 0x0061, 629 },  { 0x0062, 635 },  { 0x0063, 641 },
	{ 0x0064, 647 },  { 0x0065, 654 },  { 0x0066, 660 },  { 0x0067, 667 },
	{ 0x0068, 678 },  { 0x0069, 689 },  { 0x006a, 696 },  { 0x006b, 702 },
	{ 0x006c, 709 },  { 0x006d, 716 },  { 0x006e, 722 },  { 0x006f, 728 },
	{ 0x0070, 734 },  { 0x0071, 740 },  { 0x0072, 751 },  { 0x0073, 757 },
	{ 0x0074, 763 },  { 0x0075, 769 },  { 0x0076, 776 },  { 0x0077, 783 },
	{ 0x0078, 789 },  { 0x0079, 795 },  { 0x007a, 803 },  { 0x007c, 810 },
	{ 0x007e, 817 },  { 0x007f, 823 },  { 0x0080, 824 },  { 0x0081, 830 },
	{ 0x0082, 836 },  { 0x0083, 842 },  { 0x0084, 848 },  { 0x0085, 855 },
	{ 0x0086, 862 },  { 0x0087, 874 },  { 0x0088, 880 },  { 0x008c, 886 },
	{ 0x0091, 893 },  { 0x0092, 899 },  { 0x0400, 910 },  { 0x0401, 916 },
	{ 0x0402, 922 },  { 0x0403, 928 },  { 0x0404, 934 },  { 0x0405, 940 },
	{ 0x0406, 946 },  { 0x0407, 952 },  { 0x0408, 958 },  { 0x0409, 964 },
	{ 0x040a, 970 },  { 0x040b, 983 },  { 0x040c, 989 },  { 0x040d, 995 },
	{ 0x040e, 1001 }, { 0x040f, 1007 }, { 0x0410, 1013 }, { 0x0411, 1019 },
	{ 0x0412, 1025 }, { 0x0413, 1031 }, { 0x0414, 1037 }, { 0x0415, 1043 },
	{ 0x0416, 1049 }, { 0x0417, 1055 }, { 0x0418, 1061 }, { 0x0419, 1067 },
	{ 0x041a, 1073 }, { 0x041b, 1079 }, { 0x041c, 1085 }, { 0x041d, 1091 },
	{ 0x041e, 1097 }, { 0x041f, 1103 }, { 0x0420, 1109 }, { 0x0421, 1115 },
	{ 0x0422, 1121 }, { 0x0423, 1127 }, { 0x0424, 1133 }, { 0x0425, 1139 },
	{ 0x0426, 1145 }, { 0x0427, 1151 }, { 0x0428, 1157 }, { 0x0429, 1168 },
	{ 0x042a, 1174 }, { 0x042b, 1180 }, { 0x042c, 1186 }, { 0x042d, 1197 },
	{ 0x042e, 1203 }, { 0x042f, 1210 }, { 0x0430, 1216 }, { 0x0431, 1222 },
	{ 0x0432, 1228 }, { 0x0433, 1234 }, { 0x0434, 1240 }, { 0x0435, 1246 },
	{ 0x0436, 1252 }, { 0x0437, 1258 }, { 0x0438, 1264 }, { 0x0439, 1270 },
	{ 0x043a, 1276 }, { 0x043b, 1282 }, { 0x043d, 1288 }, { 0x043e, 1295 },
	{ 0x043f, 1301 }, { 0x0440, 1307 }, { 0x0441, 1313 }, { 0x0442, 1319 },
	{ 0x0443, 1325 }, { 0x0444, 1336 }, { 0x0445, 1342 }, { 0x0446, 1348 },
	{ 0x0447, 1354 }, { 0x0448, 1360 }, { 0x0449, 1366 }, { 0x044a, 1372 },
	{ 0x044b, 1378 }, { 0x044c, 1384 }, { 0x044d, 1390 }, { 0x044e, 1396 },
	{ 0x044f, 1402 }, { 0x0450, 1408 }, { 0x0451, 1414 }, { 0x0452, 1420 },
	{ 0x0453, 1426 }, { 0x0454, 1432 }, { 0x0455, 1438 }, { 0x0456, 1444 },
	{ 0x0457, 1450 }, { 0x0458, 1457 }, { 0x0459, 1464 }, { 0x045a, 1475 },
	{ 0x045b, 1482 }, { 0x045c, 1488 }, { 0x045d, 1500 }, { 0x045e, 1511 },
	{ 0x045f, 1517 }, { 0x0460, 1529 }, { 0x0461, 1540 }, { 0x0462, 1546 },
	{ 0x0463, 1552 }, { 0x0464, 1558 }, { 0x0465, 1565 }, { 0x0466, 1571 },
	{ 0x0467, 1578 }, { 0x0468, 1589 }, { 0x0469, 1600 }, { 0x046a, 1607 },
	{ 0x046b, 1613 }, { 0x046c, 1620 }, { 0x046d, 1627 }, { 0x046e, 1633 },
	{ 0x046f, 1639 }, { 0x0470, 1645 }, { 0x0471, 1651 }, { 0x0472, 1662 },
	{ 0x0473, 1668 }, { 0x0474, 1674 }, { 0x0475, 1680 }, { 0x0476, 1687 },
	{ 0x0477, 1694 }, { 0x0478, 1700 }, { 0x0479, 1706 }, { 0x047a, 1714 },
	{ 0x047c, 1721 }, { 0x047e, 1728 }, { 0x0480, 1734 }, { 0x0481, 1740 },
	{ 0x0482, 1746 }, { 0x0483, 1752 }, { 0x0484, 1758 }, { 0x0485, 1765 },
	{ 0x0486, 1772 }, { 0x0487, 1784 }, { 0x0488, 1790 }, { 0x048c, 1796 },
	{ 0x0491, 1803 }, { 0x0492, 1809 }, { 0x0501, 1820 }, { 0x05fe, 1829 },
	{ 0x0800, 1839 }, { 0x0801, 1845 }, { 0x0803, 1851 }, { 0x0804, 1866 },
	{ 0x0807, 1872 }, { 0x0809, 1878 }, { 0x080a, 1884 }, { 0x080c, 1890 },
	{ 0x0810, 1896 }, { 0x0813, 1902 }, { 0x0814, 1908 }, { 0x0816, 1914 },
	{ 0x0818, 1920 }, { 0x0819, 1926 }, { 0x081a, 1932 }, { 0x081d, 1943 },
	{ 0x0820, 1949 }, { 0x082c, 1955 }, { 0x082e, 1966 }, { 0x0832, 1973 },
	{ 0x083b, 1979 }, { 0x083c, 1985 }, { 0x083e, 1991 }, { 0x0843, 1997 },
	{ 0x0845, 2008 }, { 0x0846, 2014 }, { 0x0849, 2025 }, { 0x0850, 2031 },
	{ 0x0859, 2042 }, { 0x085d, 2053 }, { 0x085f, 2064 }, { 0x0860, 2076 },
	{ 0x0861, 2087 }, { 0x0867, 2093 }, { 0x086b, 2104 }, { 0x0873, 2111 },
	{ 0x0901, 2117 }, { 0x09ff, 2131 }, { 0x0c00, 2141 }, { 0x0c01, 2147 },
	{ 0x0c04, 2153 }, { 0x0c07, 2159 }, { 0x0c09, 2165 }, { 0x0c0a, 2171 },
	{ 0x0c0c, 2177 }, { 0x0c1a, 2183 }, { 0x0c3b, 2194 }, { 0x0c50, 2200 },
	{ 0x0c51, 2211 }, { 0x0c6b, 2217 }, { 0x1000, 2224 }, { 0x1001, 2235 },
	{ 0x1004, 2241 }, { 0x1007, 2247 }, { 0x1009, 2253 }, { 0x100a, 2259 },
	{ 0x100c, 2265 }, { 0x101a, 2271 }, { 0x103b, 2277 }, { 0x105f, 2284 },
	{ 0x1401, 2296 }, { 0x1404, 2302 }, { 0x1407, 2308 }, { 0x1409, 2314 },
	{ 0x140a, 2320 }, { 0x140c, 2326 }, { 0x141a, 2332 }, { 0x143b, 2343 },
	{ 0x1801, 2350 }, { 0x1809, 2356 }, { 0x180a, 2362 }, { 0x180c, 2368 },
	{ 0x181a, 2374 }, { 0x183b, 2385 }, { 0x1c01, 2392 }, { 0x1c09, 2398 },
	{ 0x1c0a, 2404 }, { 0x1c0c, 2410 }, { 0x1c1a, 2417 }, { 0x1c3b, 2428 },
	{ 0x2000, 2435 }, { 0x2001, 2441 }, { 0x2009, 2447 }, { 0x200a, 2453 },
	{ 0x200c, 2459 }, { 0x201a, 2465 }, { 0x203b, 2476 }, { 0x2400, 2483 },
	{ 0x2401, 2489 }, { 0x2409, 2495 }, { 0x240a, 2502 }, { 0x240c, 2508 },
	{ 0x241a, 2514 }, { 0x243b, 2525 }, { 0x2800, 2532 }, { 0x2801, 2538 },
	{ 0x2809, 2544 }, { 0x280a, 2550 }, { 0x280c, 2556 }, { 0x281a, 2562 },
	{ 0x2c00, 2573 }, { 0x2c01, 2579 }, { 0x2c09, 2585 }, { 0x2c0a, 2591 },
	{ 0x2c0c, 2597 }, { 0x2c1a, 2603 }, { 0x3000, 2614 }, { 0x3001, 2620 },
	{ 0x3009, 2626 }, { 0x300a, 2632 }, { 0x300c, 2638 }, { 0x301a, 2644 },
	{ 0x3400, 2655 }, { 0x3401, 2661 }, { 0x3409, 2667 }, { 0x340a, 2673 },
	{ 0x340c, 2679 }, { 0x3800, 2685 }, { 0x3801, 2691 }, { 0x3809, 2697 },
	{ 0x380a, 2703 }, { 0x380c, 2709 }, { 0x3c00, 2715 }, { 0x3c01, 2721 },
	{ 0x3c09, 2727 }, { 0x3c0a, 2733 }, { 0x3c0c, 2739 }, { 0x4000, 2745 },
	{ 0x4001, 2751 }, { 0x4009, 2757 }, { 0x400a, 2763 }, { 0x4400, 2769 },
	{ 0x4409, 2775 }, { 0x440a, 2781 }, { 0x4800, 2787 }, { 0x4809, 2793 },
	{ 0x480a, 2799 }, { 0x4c00, 2805 }, { 0x4c09, 2811 }, { 0x4c0a, 2817 },
	{ 0x500a, 2823 }, { 0x540a, 2829 }, { 0x580a, 2835 }, { 0x5c0a, 2842 },
	{ 0x641a, 2848 }, { 0x681a, 2859 }, { 0x6c1a, 2870 }, { 0x701a, 2881 },
	{ 0x703b, 2892 }, { 0x742c, 2899 }, { 0x743b, 2910 }, { 0x7804, 2917 },
	{ 0x7814, 2923 }, { 0x781a, 2929 }, { 0x782c, 2940 }, { 0x783b, 2951 },
	{ 0x7843, 2958 }, { 0x7850, 2969 }, { 0x785d, 2975 }, { 0x785f, 2986 },
	{ 0x7c04, 2998 }, { 0x7c14, 3004 }, { 0x7c1a, 3010 }, { 0x7c28, 3021 },
	{ 0x7c2e, 3032 }, { 0x7c3b, 3039 }, { 0x7c43, 3046 }, { 0x7c46, 3057 },
	{ 0x7c50, 3068 }, { 0x7c59, 3079 }, { 0x7c5c, 3090 }, { 0x7c5d, 3102 },
	{ 0x7c5f, 3113 }, { 0x7c67, 3125 }, { 0x7c68, 3136 }, { 0x7c86, 3147 },
	{ 0x7c92, 3159 },
};

/* All the language names; generated by tools/generate_language_id_map.c.
 * For compactness, this is a 'char' string rather than a 'tchar' string.  */
static const char language_names[3170] =
	"en-US\0ar-SA\0bg-BG\0ca-ES\0zh-CN\0cs-CZ\0da-DK\0de-DE\0el-GR\0en-US\0"
	"es-ES\0fi-FI\0fr-FR\0he-IL\0hu-HU\0is-IS\0it-IT\0ja-JP\0ko-KR\0nl-NL\0"
	"nb-NO\0pl-PL\0pt-BR\0rm-CH\0ro-RO\0ru-RU\0hr-HR\0sk-SK\0sq-AL\0sv-SE\0"
	"th-TH\0tr-TR\0ur-PK\0id-ID\0uk-UA\0be-BY\0sl-SI\0et-EE\0lv-LV\0lt-LT\0"
	"tg-Cyrl-TJ\0fa-IR\0vi-VN\0hy-AM\0az-Latn-AZ\0eu-ES\0hsb-DE\0mk-MK\0"
	"st-ZA\0ts-ZA\0tn-ZA\0ve-ZA\0xh-ZA\0zu-ZA\0af-ZA\0ka-GE\0fo-FO\0hi-IN\0"
	"mt-MT\0se-NO\0ga-IE\0yi-001\0ms-MY\0kk-KZ\0ky-KG\0sw-KE\0tk-TM\0"
	"uz-Latn-UZ\0tt-RU\0bn-BD\0pa-IN\0gu-IN\0or-IN\0ta-IN\0te-IN\0kn-IN\0"
	"ml-IN\0as-IN\0mr-IN\0sa-IN\0mn-MN\0bo-CN\0cy-GB\0km-KH\0lo-LA\0my-MM\0"
	"gl-ES\0kok-IN\0mni-IN\0sd-Arab-PK\0syr-SY\0si-LK\0chr-Cher-US\0"
	"iu-Latn-CA\0am-ET\0tzm-Latn-DZ\0ks-Arab-IN\0ne-NP\0fy-NL\0ps-AF\0"
	"fil-PH\0dv-MV\0bin-NG\0ff-Latn-SN\0ha-Latn-NG\0ibb-NG\0yo-NG\0quz-BO\0"
	"nso-ZA\0ba-RU\0lb-LU\0kl-GL\0ig-NG\0kr-Latn-NG\0om-ET\0ti-ER\0gn-PY\0"
	"haw-US\0la-001\0so-SO\0ii-CN\0pap-029\0arn-CL\0moh-CA\0br-FR\0\0"
	"ug-CN\0mi-NZ\0oc-FR\0co-FR\0gsw-CH\0sah-RU\0quc-Latn-GT\0rw-RW\0"
	"wo-SN\0prs-AF\0gd-GB\0ku-Arab-IQ\0en-US\0ar-SA\0bg-BG\0ca-ES\0zh-TW\0"
	"cs-CZ\0da-DK\0de-DE\0el-GR\0en-US\0es-ES_tradnl\0fi-FI\0fr-FR\0he-IL\0"
	"hu-HU\0is-IS\0it-IT\0ja-JP\0ko-KR\0nl-NL\0nb-NO\0pl-PL\0pt-BR\0rm-CH\0"
	"ro-RO\0ru-RU\0hr-HR\0sk-SK\0sq-AL\0sv-SE\0th-TH\0tr-TR\0ur-PK\0id-ID\0"
	"uk-UA\0be-BY\0sl-SI\0et-EE\0lv-LV\0lt-LT\0tg-Cyrl-TJ\0fa-IR\0vi-VN\0"
	"hy-AM\0az-Latn-AZ\0eu-ES\0hsb-DE\0mk-MK\0st-ZA\0ts-ZA\0tn-ZA\0ve-ZA\0"
	"xh-ZA\0zu-ZA\0af-ZA\0ka-GE\0fo-FO\0hi-IN\0mt-MT\0se-NO\0yi-001\0"
	"ms-MY\0kk-KZ\0ky-KG\0sw-KE\0tk-TM\0uz-Latn-UZ\0tt-RU\0bn-IN\0pa-IN\0"
	"gu-IN\0or-IN\0ta-IN\0te-IN\0kn-IN\0ml-IN\0as-IN\0mr-IN\0sa-IN\0mn-MN\0"
	"bo-CN\0cy-GB\0km-KH\0lo-LA\0my-MM\0gl-ES\0kok-IN\0mni-IN\0sd-Deva-IN\0"
	"syr-SY\0si-LK\0chr-Cher-US\0iu-Cans-CA\0am-ET\0tzm-Arab-MA\0"
	"ks-Arab-IN\0ne-NP\0fy-NL\0ps-AF\0fil-PH\0dv-MV\0bin-NG\0ff-Latn-NG\0"
	"ha-Latn-NG\0ibb-NG\0yo-NG\0quz-BO\0nso-ZA\0ba-RU\0lb-LU\0kl-GL\0"
	"ig-NG\0kr-Latn-NG\0om-ET\0ti-ET\0gn-PY\0haw-US\0la-001\0so-SO\0ii-CN\0"
	"pap-029\0arn-CL\0moh-CA\0br-FR\0ug-CN\0mi-NZ\0oc-FR\0co-FR\0gsw-FR\0"
	"sah-RU\0quc-Latn-GT\0rw-RW\0wo-SN\0prs-AF\0gd-GB\0ku-Arab-IQ\0"
	"qps-ploc\0qps-ploca\0en-US\0ar-IQ\0ca-ES-valencia\0zh-CN\0de-CH\0"
	"en-GB\0es-MX\0fr-BE\0it-CH\0nl-BE\0nn-NO\0pt-PT\0ro-MD\0ru-MD\0"
	"sr-Latn-CS\0sv-FI\0ur-IN\0az-Cyrl-AZ\0dsb-DE\0tn-BW\0se-SE\0ga-IE\0"
	"ms-BN\0uz-Cyrl-UZ\0bn-BD\0pa-Arab-PK\0ta-LK\0mn-Mong-CN\0sd-Arab-PK\0"
	"iu-Latn-CA\0tzm-Latn-DZ\0ks-Deva-IN\0ne-IN\0ff-Latn-SN\0quz-EC\0"
	"ti-ER\0qps-Latn-x-sh\0qps-plocm\0en-US\0ar-EG\0zh-HK\0de-AT\0en-AU\0"
	"es-ES\0fr-CA\0sr-Cyrl-CS\0se-FI\0mn-Mong-MN\0dz-BT\0quz-PE\0"
	"ks-Arab-IN\0ar-LY\0zh-SG\0de-LU\0en-CA\0es-GT\0fr-CH\0hr-BA\0smj-NO\0"
	"tzm-Tfng-MA\0ar-DZ\0zh-MO\0de-LI\0en-NZ\0es-CR\0fr-LU\0bs-Latn-BA\0"
	"smj-SE\0ar-MA\0en-IE\0es-PA\0fr-MC\0sr-Latn-BA\0sma-NO\0ar-TN\0en-ZA\0"
	"es-DO\0fr-029\0sr-Cyrl-BA\0sma-SE\0en-US\0ar-OM\0en-JM\0es-VE\0fr-RE\0"
	"bs-Cyrl-BA\0sms-FI\0en-US\0ar-YE\0en-029\0es-CO\0fr-CD\0sr-Latn-RS\0"
	"smn-FI\0en-US\0ar-SY\0en-BZ\0es-PE\0fr-SN\0sr-Cyrl-RS\0en-US\0ar-JO\0"
	"en-TT\0es-AR\0fr-CM\0sr-Latn-ME\0en-US\0ar-LB\0en-ZW\0es-EC\0fr-CI\0"
	"sr-Cyrl-ME\0en-US\0ar-KW\0en-PH\0es-CL\0fr-ML\0en-US\0ar-AE\0en-ID\0"
	"es-UY\0fr-MA\0en-US\0ar-BH\0en-HK\0es-PY\0fr-HT\0en-US\0ar-QA\0en-IN\0"
	"es-BO\0en-US\0en-MY\0es-SV\0en-US\0en-SG\0es-HN\0en-US\0en-AE\0es-NI\0"
	"es-PR\0es-US\0es-419\0es-CU\0bs-Cyrl-BA\0bs-Latn-BA\0sr-Cyrl-RS\0"
	"sr-Latn-RS\0smn-FI\0az-Cyrl-AZ\0sms-FI\0zh-CN\0nn-NO\0bs-Latn-BA\0"
	"az-Latn-AZ\0sma-SE\0uz-Cyrl-UZ\0mn-MN\0iu-Cans-CA\0tzm-Tfng-MA\0"
	"zh-HK\0nb-NO\0sr-Latn-RS\0tg-Cyrl-TJ\0dsb-DE\0smj-SE\0uz-Latn-UZ\0"
	"pa-Arab-PK\0mn-Mong-CN\0sd-Arab-PK\0chr-Cher-US\0iu-Latn-CA\0"
	"tzm-Latn-DZ\0ff-Latn-SN\0ha-Latn-NG\0quc-Latn-GT\0ku-Arab-IQ\0";

/* Translate a Windows language ID to its name.  Returns NULL if the ID is not
 * recognized.  */
static const char *
language_id_to_name(u16 id)
{
	int l = 0;
	int r = ARRAY_LEN(language_id_map) - 1;
	do {
		int m = (l + r) / 2;
		if (id < language_id_map[m].id)
			r = m - 1;
		else if (id > language_id_map[m].id)
			l = m + 1;
		else
			return &language_names[language_id_map[m]
			                               .name_start_offset];
	} while (l <= r);
	return NULL;
}

/* PE binary processor architecture codes (common ones only)  */
#define IMAGE_FILE_MACHINE_I386  0x014C
#define IMAGE_FILE_MACHINE_ARM   0x01C0
#define IMAGE_FILE_MACHINE_ARMV7 0x01C4
#define IMAGE_FILE_MACHINE_THUMB 0x01C2
#define IMAGE_FILE_MACHINE_IA64  0x0200
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_MACHINE_ARM64 0xAA64

/* Windows API processor architecture codes (common ones only)  */
#define PROCESSOR_ARCHITECTURE_INTEL 0
#define PROCESSOR_ARCHITECTURE_ARM   5
#define PROCESSOR_ARCHITECTURE_IA64  6
#define PROCESSOR_ARCHITECTURE_AMD64 9
#define PROCESSOR_ARCHITECTURE_ARM64 12

/* Translate a processor architecture code as given in a PE binary to the code
 * used by the Windows API.  Returns -1 if the code is not recognized.  */
static int
pe_arch_to_windows_arch(unsigned pe_arch)
{
	switch (pe_arch) {
	case IMAGE_FILE_MACHINE_I386:
		return PROCESSOR_ARCHITECTURE_INTEL;
	case IMAGE_FILE_MACHINE_ARM:
	case IMAGE_FILE_MACHINE_ARMV7:
	case IMAGE_FILE_MACHINE_THUMB:
		return PROCESSOR_ARCHITECTURE_ARM;
	case IMAGE_FILE_MACHINE_IA64:
		return PROCESSOR_ARCHITECTURE_IA64;
	case IMAGE_FILE_MACHINE_AMD64:
		return PROCESSOR_ARCHITECTURE_AMD64;
	case IMAGE_FILE_MACHINE_ARM64:
		return PROCESSOR_ARCHITECTURE_ARM64;
	}
	return -1;
}

/* Gather information from kernel32.dll.  */
static void
set_info_from_kernel32(struct windows_info_ctx *ctx,
                       const void *contents,
                       size_t size)
{
	u32 e_lfanew;
	const u8 *pe_hdr;
	unsigned pe_arch;
	int arch;

	/* Read the processor architecture from the executable header.  */

	if (size < 0x40)
		goto invalid;

	e_lfanew = le32_to_cpu(*(le32 *)((u8 *)contents + 0x3C));
	if (e_lfanew > size || size - e_lfanew < 6 || (e_lfanew & 3))
		goto invalid;

	pe_hdr = (u8 *)contents + e_lfanew;
	if (*(le32 *)pe_hdr != cpu_to_le32(0x00004550)) /* "PE\0\0"  */
		goto invalid;

	pe_arch = le16_to_cpu(*(le16 *)(pe_hdr + 4));
	arch    = pe_arch_to_windows_arch(pe_arch);
	if (arch >= 0) {
		/* Save the processor architecture in the XML document.  */
		set_number_property(ctx, T("WINDOWS/ARCH"), arch);
	} else {
		XML_WARN("Architecture value %x from kernel32.dll "
		         "header not recognized",
		         pe_arch);
	}
	return;

invalid:
	XML_WARN("kernel32.dll is not a valid PE binary.");
}

/* Gather information from the SOFTWARE registry hive.  */
static void
set_info_from_software_hive(struct windows_info_ctx *ctx,
                            const struct regf *regf)
{
	const tchar *version_key = T("Microsoft\\Windows NT\\CurrentVersion");
	s64 major_version        = -1;
	s64 minor_version        = -1;
	tchar *version_string;
	tchar *build_string;

	/* Image flags  */
	copy_registry_string(
		ctx, regf, version_key, T("EditionID"), T("FLAGS"));

	/* Image display name  */
	copy_registry_string(
		ctx, regf, version_key, T("ProductName"), T("DISPLAYNAME"));

	/* Image display description  */
	copy_registry_string(ctx,
	                     regf,
	                     version_key,
	                     T("ProductName"),
	                     T("DISPLAYDESCRIPTION"));

	/* Edition ID  */
	copy_registry_string(
		ctx, regf, version_key, T("EditionID"), T("WINDOWS/EDITIONID"));

	/* Installation type  */
	copy_registry_string(ctx,
	                     regf,
	                     version_key,
	                     T("InstallationType"),
	                     T("WINDOWS/INSTALLATIONTYPE"));

	/* Product name  */
	copy_registry_string(ctx,
	                     regf,
	                     version_key,
	                     T("ProductName"),
	                     T("WINDOWS/PRODUCTNAME"));

	/* Major and minor version number  */

	/* Note: in Windows 10, CurrentVersion was apparently fixed at 6.3.
	 * Instead, the new values CurrentMajorVersionNumber and
	 * CurrentMinorVersionNumber should be used.  */

	get_number_from_registry(ctx,
	                         regf,
	                         version_key,
	                         T("CurrentMajorVersionNumber"),
	                         &major_version);

	get_number_from_registry(ctx,
	                         regf,
	                         version_key,
	                         T("CurrentMinorVersionNumber"),
	                         &minor_version);

	if (major_version < 0 || minor_version < 0) {
		if (get_string_from_registry(ctx,
		                             regf,
		                             version_key,
		                             T("CurrentVersion"),
		                             &version_string))
		{
			if (2 != tscanf(version_string,
			                T("%" PRIi64 ".%" PRIi64),
			                &major_version,
			                &minor_version))
			{
				XML_WARN("Unrecognized CurrentVersion: %" TS,
				         version_string);
			}
			FREE(version_string);
		}
	}

	if (major_version >= 0) {
		set_number_property(
			ctx, T("WINDOWS/VERSION/MAJOR"), major_version);
		if (minor_version >= 0) {
			set_number_property(
				ctx, T("WINDOWS/VERSION/MINOR"), minor_version);
		}
	}

	/* Build number  */

	/* Note: "CurrentBuild" is marked as obsolete in Windows XP registries
	 * (example value:  "1.511.1 () (Obsolete data - do not use)"), and
	 * "CurrentBuildNumber" contains the correct value.  But oddly enough,
	 * it is "CurrentBuild" that contains the correct value on *later*
	 * versions of Windows.  */
	if (get_string_from_registry(
		    ctx, regf, version_key, T("CurrentBuild"), &build_string))
	{
		if (tstrchr(build_string, T('.'))) {
			FREE(build_string);
			build_string = NULL;
			get_string_from_registry(ctx,
			                         regf,
			                         version_key,
			                         T("CurrentBuildNumber"),
			                         &build_string);
		}
		if (build_string) {
			set_string_property(
				ctx, T("WINDOWS/VERSION/BUILD"), build_string);
			FREE(build_string);
		}
	}
}

/* Gather the default language from the SYSTEM registry hive.  */
static void
set_default_language(struct windows_info_ctx *ctx, const struct regf *regf)
{
	tchar *string;
	unsigned language_id;

	if (!get_string_from_registry(ctx,
	                              regf,
	                              T("ControlSet001\\Control\\Nls\\Language"),
	                              T("InstallLanguage"),
	                              &string))
		return;

	if (1 == tscanf(string, T("%x"), &language_id)) {
		const char *language_name = language_id_to_name(language_id);
		if (language_name) {
			size_t len = strlen(language_name);
			tchar tstr[len + 1];
			for (size_t i = 0; i <= len; i++)
				tstr[i] = language_name[i];
			set_string_property(
				ctx, T("WINDOWS/LANGUAGES/DEFAULT"), tstr);
			FREE(string);
			return;
		}
	}
	XML_WARN("Unrecognized InstallLanguage: %" TS, string);
	FREE(string);
}

/* Gather information from the SYSTEM registry hive.  */
static void
set_info_from_system_hive(struct windows_info_ctx *ctx, const struct regf *regf)
{
	const tchar *windows_key = T("ControlSet001\\Control\\Windows");
	const tchar *uilanguages_key =
		T("ControlSet001\\Control\\MUI\\UILanguages");
	const tchar *productoptions_key =
		T("ControlSet001\\Control\\ProductOptions");
	s64 spbuild;
	s64 splevel;
	tchar **subkeys;

	/* Service pack build  */
	if (get_number_from_registry(
		    ctx, regf, windows_key, T("CSDBuildNumber"), &spbuild))
		set_number_property(ctx, T("WINDOWS/VERSION/SPBUILD"), spbuild);

	/* Service pack level  */
	if (get_number_from_registry(
		    ctx, regf, windows_key, T("CSDVersion"), &splevel))
		set_number_property(
			ctx, T("WINDOWS/VERSION/SPLEVEL"), splevel >> 8);

	/* Product type  */
	copy_registry_string(ctx,
	                     regf,
	                     productoptions_key,
	                     T("ProductType"),
	                     T("WINDOWS/PRODUCTTYPE"));

	/* Product suite  */
	copy_registry_string(ctx,
	                     regf,
	                     productoptions_key,
	                     T("ProductSuite"),
	                     T("WINDOWS/PRODUCTSUITE"));

	/* Hardware abstraction layer  */
	copy_registry_string(
		ctx,
		regf,
		T("ControlSet001\\Control\\Class\\{4D36E966-E325-11CE-BFC1-08002BE10318}\\0000"),
		T("MatchingDeviceId"),
		T("WINDOWS/HAL"));

	/* Languages  */
	if (list_subkeys_in_registry(ctx, regf, uilanguages_key, &subkeys)) {
		tchar property_name[64];
		for (tchar **p = subkeys; *p; p++) {
			tsprintf(property_name,
			         T("WINDOWS/LANGUAGES/LANGUAGE[%zu]"),
			         p - subkeys + 1);
			set_string_property(ctx, property_name, *p);
		}
		hive_free_subkeys_list(subkeys);
	}

	/* Default language  */
	set_default_language(ctx, regf);
}

/* Load the contents of a file in the currently selected WIM image into memory.
 */
static void *
load_file_contents(struct windows_info_ctx *ctx,
                   const struct wim_dentry *dentry,
                   const char *filename,
                   size_t *size_ret)
{
	const struct blob_descriptor *blob;
	void *contents;
	int ret;

	if (!dentry) {
		XML_WARN("%s does not exist", filename);
		return NULL;
	}

	blob = inode_get_blob_for_unnamed_data_stream(dentry->d_inode,
	                                              ctx->wim->blob_table);
	if (!blob) {
		XML_WARN("%s has no contents", filename);
		return NULL;
	}

	ret = read_blob_into_alloc_buf(blob, &contents);
	if (ret) {
		XML_WARN("Error loading %s (size=%" PRIu64 "): %" TS,
		         filename,
		         blob->size,
		         wimlib_get_error_string(ret));
		ctx->oom_encountered |=
			(ret == WIMLIB_ERR_NOMEM && blob->size < 100000000);
		return NULL;
	}

	*size_ret = blob->size;
	return contents;
}

/* Load and validate a registry hive file.  */
static void *
load_hive(struct windows_info_ctx *ctx,
          const struct wim_dentry *dentry,
          const char *filename)
{
	void *hive_mem;
	size_t hive_size;

	hive_mem = load_file_contents(ctx, dentry, filename, &hive_size);
	if (hive_mem && !is_registry_valid(ctx, hive_mem, hive_size)) {
		XML_WARN("%s is not a valid registry hive!", filename);
		FREE(hive_mem);
		hive_mem = NULL;
	}
	return hive_mem;
}

/* Set the WINDOWS/SYSTEMROOT property to the name of the directory specified by
 * 'systemroot'.  */
static void
set_systemroot_property(struct windows_info_ctx *ctx,
                        const struct wim_dentry *systemroot)
{
	utf16lechar *uname;
	const tchar *name;
	size_t name_nbytes;
	int ret;

	/* to uppercase ...  */
	uname = utf16le_dupz(systemroot->d_name, systemroot->d_name_nbytes);
	if (!uname) {
		ctx->oom_encountered = true;
		goto out;
	}
	for (size_t i = 0; i < systemroot->d_name_nbytes / 2; i++)
		uname[i] = cpu_to_le16(upcase[le16_to_cpu(uname[i])]);

	/* to tstring ...  */
	ret = utf16le_get_tstr(
		uname, systemroot->d_name_nbytes, &name, &name_nbytes);
	if (ret) {
		ctx->oom_encountered |= (ret == WIMLIB_ERR_NOMEM);
		XML_WARN("Failed to get systemroot name: %" TS,
		         wimlib_get_error_string(ret));
		goto out;
	}
	set_string_property(ctx, T("WINDOWS/SYSTEMROOT"), name);
	utf16le_put_tstr(name);
out:
	FREE(uname);
}

static int
do_set_windows_specific_info(WIMStruct *wim,
                             const struct wim_dentry *systemroot,
                             const struct wim_dentry *kernel32,
                             const struct wim_dentry *software,
                             const struct wim_dentry *system)
{
	void *contents;
	size_t size;
	struct windows_info_ctx _ctx = {
		.wim = wim,
		.image = wim->current_image,
		.oom_encountered = false,
		.debug_enabled = (tgetenv(T("WIMLIB_DEBUG_XML_INFO")) != NULL),
	}, *ctx = &_ctx;

	set_systemroot_property(ctx, systemroot);

	if ((contents =
	             load_file_contents(ctx, kernel32, "kernel32.dll", &size)))
	{
		set_info_from_kernel32(ctx, contents, size);
		FREE(contents);
	}

	if ((contents = load_hive(ctx, software, "SOFTWARE"))) {
		set_info_from_software_hive(ctx, contents);
		FREE(contents);
	}

	if ((contents = load_hive(ctx, system, "SYSTEM"))) {
		set_info_from_system_hive(ctx, contents);
		FREE(contents);
	}

	if (ctx->oom_encountered) {
		ERROR("Ran out of memory while setting Windows-specific "
		      "metadata in the WIM file's XML document.");
		return WIMLIB_ERR_NOMEM;
	}

	return 0;
}

/* Windows */
static const utf16lechar windows_name[] = {
	cpu_to_le16('W'), cpu_to_le16('i'), cpu_to_le16('n'), cpu_to_le16('d'),
	cpu_to_le16('o'), cpu_to_le16('w'), cpu_to_le16('s'),
};

/* System32 */
static const utf16lechar system32_name[] = {
	cpu_to_le16('S'), cpu_to_le16('y'), cpu_to_le16('s'), cpu_to_le16('t'),
	cpu_to_le16('e'), cpu_to_le16('m'), cpu_to_le16('3'), cpu_to_le16('2'),
};

/* kernel32.dll */
static const utf16lechar kernel32_name[] = {
	cpu_to_le16('k'), cpu_to_le16('e'), cpu_to_le16('r'), cpu_to_le16('n'),
	cpu_to_le16('e'), cpu_to_le16('l'), cpu_to_le16('3'), cpu_to_le16('2'),
	cpu_to_le16('.'), cpu_to_le16('d'), cpu_to_le16('l'), cpu_to_le16('l'),
};

/* config */
static const utf16lechar config_name[] = {
	cpu_to_le16('c'), cpu_to_le16('o'), cpu_to_le16('n'),
	cpu_to_le16('f'), cpu_to_le16('i'), cpu_to_le16('g'),
};

/* SOFTWARE */
static const utf16lechar software_name[] = {
	cpu_to_le16('S'), cpu_to_le16('O'), cpu_to_le16('F'), cpu_to_le16('T'),
	cpu_to_le16('W'), cpu_to_le16('A'), cpu_to_le16('R'), cpu_to_le16('E'),
};

/* SYSTEM */
static const utf16lechar system_name[] = {
	cpu_to_le16('S'), cpu_to_le16('Y'), cpu_to_le16('S'),
	cpu_to_le16('T'), cpu_to_le16('E'), cpu_to_le16('M'),
};

#define GET_CHILD(parent, child_name) \
  get_dentry_child_with_utf16le_name( \
	  parent, child_name, sizeof(child_name), WIMLIB_CASE_INSENSITIVE)

static bool
is_default_systemroot(const struct wim_dentry *potential_systemroot)
{
	return !cmp_utf16le_strings(potential_systemroot->d_name,
	                            potential_systemroot->d_name_nbytes / 2,
	                            windows_name,
	                            ARRAY_LEN(windows_name),
	                            true);
}

/*
 * Set Windows-specific XML information for the currently selected WIM image.
 *
 * This process is heavily based on heuristics and hard-coded logic related to
 * where Windows stores certain types of information.  Therefore, it simply
 * tries to set as much information as possible.  If there's a problem, it skips
 * the affected information and proceeds to the next part.  It only returns an
 * error code if there was a severe problem such as out-of-memory.
 */
int
set_windows_specific_info(WIMStruct *wim)
{
	const struct wim_dentry *root, *potential_systemroot,
		*best_systemroot = NULL, *best_kernel32 = NULL,
		*best_software = NULL, *best_system = NULL;
	int best_score = 0;

	root = wim_get_current_root_dentry(wim);
	if (!root)
		return 0;

	/* Find the system root.  This is usually the toplevel directory
	 * "Windows", but it might be a different toplevel directory.  Choose
	 * the directory that contains the greatest number of the files we want:
	 * System32/kernel32.dll, System32/config/SOFTWARE, and
	 * System32/config/SYSTEM.  Compare all names case insensitively.  */
	for_dentry_child(potential_systemroot, root)
	{
		const struct wim_dentry *system32, *kernel32, *config,
			*software = NULL, *system = NULL;
		int score;

		if (!dentry_is_directory(potential_systemroot))
			continue;
		system32 = GET_CHILD(potential_systemroot, system32_name);
		if (!system32)
			continue;
		kernel32 = GET_CHILD(system32, kernel32_name);
		config   = GET_CHILD(system32, config_name);
		if (config) {
			software = GET_CHILD(config, software_name);
			system   = GET_CHILD(config, system_name);
		}

		score = !!kernel32 + !!software + !!system;
		if (score >= best_score) {
			/* If there's a tie, prefer the "Windows" directory.  */
			if (score > best_score ||
			    is_default_systemroot(potential_systemroot))
			{
				best_score      = score;
				best_systemroot = potential_systemroot;
				best_kernel32   = kernel32;
				best_software   = software;
				best_system     = system;
			}
		}
	}

	if (likely(best_score == 0))
		return 0; /* No Windows system root found.  */

	/* Found the Windows system root.  */
	return do_set_windows_specific_info(wim,
	                                    best_systemroot,
	                                    best_kernel32,
	                                    best_software,
	                                    best_system);
}
