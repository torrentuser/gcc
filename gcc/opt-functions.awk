#  Copyright (C) 2003-2025 Free Software Foundation, Inc.
#  Contributed by Kelley Cook, June 2004.
#  Original code from Neil Booth, May 2003.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 3, or (at your option) any
# later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING3.  If not see
# <http://www.gnu.org/licenses/>.

# Some common subroutines for use by opt[ch]-gen.awk.

# Define some helpful character classes, for portability.
BEGIN {
	lower = "abcdefghijklmnopqrstuvwxyz"
	upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	digit = "0123456789"
	alnum = lower "" upper "" digit
}

# Return nonzero if FLAGS contains a flag matching REGEX.
function flag_set_p(regex, flags)
{
    # Ignore the arguments of flags with arguments.
    gsub ("\\([^)]+\\)", "", flags);
    return (" " flags " ") ~ (" " regex " ")
}

# Return STRING if FLAGS contains a flag matching regexp REGEX,
# otherwise return the empty string.
function test_flag(regex, flags, string)
{
	if (flag_set_p(regex, flags))
		return string
	return ""
}

# Return a field initializer, with trailing comma, for a field that is
# 1 if FLAGS contains a flag matching REGEX and 0 otherwise.
function flag_init(regex, flags)
{
	if (flag_set_p(regex, flags))
		return "1 /* " regex " */, "
	else
		return "0, "
}

# If FLAGS contains a "NAME(...argument...)" flag, return the value
# of the argument.  Return the empty string otherwise.
function opt_args(name, flags)
{
	flags = " " flags
	if (flags !~ " " name "\\(")
		return ""
	sub(".* " name "\\(", "", flags)
	if (flags ~ "^[{]")
	{
		sub ("^[{]", "", flags)
		sub ("}\\).*", "", flags)
	}
	else
		sub("\\).*", "", flags)

	return flags
}

# If FLAGS contains a "NAME(...argument...)" flag, return the value
# of the argument.  Print error message otherwise.
function opt_args_non_empty(name, flags, description)
{
	args = opt_args(name, flags)
	if (args == "")
		print "#error Empty option argument '" name "' during parsing of: " flags
	return args
}

# Return the number of comma-separated element of S.
function n_args(s)
{
	n = 1
	while (s ~ ",") {
		n++
		sub("[^,]*, *", "", s)
	}
	return n
}

# Return the Nth comma-separated element of S.  Return the empty string
# if S does not contain N elements.
function nth_arg(n, s)
{
	while (n-- > 0) {
		if (s !~ ",")
			return ""
		sub("[^,]*, *", "", s)
	}
	sub(",.*", "", s)
	return s
}

# Return a bitmask of CL_* values for option flags FLAGS.
function switch_flags (flags)
{
	result = "0"
	for (j = 0; j < n_langs; j++) {
		regex = langs[j]
		gsub ( "\\+", "\\+", regex )
		result = result test_flag(regex, flags, " | " macros[j])
	}
	result = result \
	  test_flag("Common", flags, " | CL_COMMON") \
	  test_flag("Target", flags, " | CL_TARGET") \
	  test_flag("PchIgnore", flags, " | CL_PCH_IGNORE") \
	  test_flag("Driver", flags, " | CL_DRIVER") \
	  test_flag("Joined", flags, " | CL_JOINED") \
	  test_flag("JoinedOrMissing", flags, " | CL_JOINED") \
	  test_flag("Separate", flags, " | CL_SEPARATE") \
	  test_flag("Undocumented", flags,  " | CL_UNDOCUMENTED") \
	  test_flag("NoDWARFRecord", flags,  " | CL_NO_DWARF_RECORD") \
	  test_flag("Warning", flags,  " | CL_WARNING") \
	  test_flag("(Optimization|PerFunction)", flags,  " | CL_OPTIMIZATION") \
	  test_flag("Param", flags,  " | CL_PARAMS")
	sub( "^0 \\| ", "", result )
	return result
}

# Return bit-field initializers for option flags FLAGS.
function switch_bit_fields (flags)
{
	uinteger_flag = ""
	vn = var_name(flags);
	if (host_wide_int[vn] == "yes")
		hwi = "Host_Wide_Int"
	else if (flag_set_p("Host_Wide_Int", flags)) {
		hwi = "Host_Wide_Int"
		uinteger_flag = flag_init("UInteger", flags)
	}
	else
		hwi = ""
	result = ""
	sep_args = opt_args("Args", flags)
	if (sep_args == "")
		sep_args = 0
	else
		sep_args--
	result = result sep_args ", "

	if (uinteger_flag == "")
		uinteger_flag = flag_init("UInteger", flags)

	hwi_flag = flag_init("Host_Wide_Int", hwi)
	byte_size_flag = flag_init("ByteSize", flags)

	if (substr(byte_size_flag, 1, 1) != "0"	\
	    && substr(uinteger_flag, 1, 1) == "0" \
	    && substr(hwi_flag, 1, 1) == "0")
	  print "#error only UInteger amd Host_Wide_Int options can specify a ByteSize suffix"

	# The following flags need to be in the same order as
	# the corresponding members of struct cl_option defined
	# in gcc/opts.h.
	result = result \
	  flag_init("SeparateAlias", flags) \
	  flag_init("NegativeAlias", flags) \
	  flag_init("NoDriverArg", flags) \
	  flag_init("RejectDriver", flags) \
	  flag_init("RejectNegative", flags) \
	  flag_init("JoinedOrMissing", flags) \
	  uinteger_flag \
	  hwi_flag \
	  flag_init("ToLower", flags) \
	  byte_size_flag

	if (flag_set_p("Report", flags))
	    print "#error Report option property is dropped"

	sub(", $", "", result)
	return result
}

# If FLAGS includes a Var flag, return the name of the variable it specifies.
# Return the empty string otherwise.
function var_name(flags)
{
	return nth_arg(0, opt_args("Var", flags))
}

# If FLAGS includes a UrlSuffix flag, return the value it specifies.
# Return the empty string otherwise.
function url_suffix(flags)
{
	return nth_arg(0, opt_args("UrlSuffix", flags))
}

# If FLAGS includes a LangUrlSuffix_LANG flag, return the
# value it specifies.
# Return the empty string otherwise.
function lang_url_suffix(flags, lang)
{
	return nth_arg(0, opt_args("LangUrlSuffix_" lang, flags))
}

# Return the name of the variable if FLAGS has a HOST_WIDE_INT variable. 
# Return the empty string otherwise.
function host_wide_int_var_name(flags)
{
	split (flags, array, "[ \t]+")
	if (array[1] == "HOST_WIDE_INT")
		return array[2]
	else
		return ""
}

# Return true if the option described by FLAGS has a globally-visible state.
function global_state_p(flags)
{
	return (var_name(flags) != "" \
		|| opt_args("Mask", flags) != "" \
		|| opt_args("InverseMask", flags) != "")
}

# Return true if the option described by FLAGS must have some state
# associated with it.
function needs_state_p(flags)
{
	return (flag_set_p("Target", flags) \
		&& !flag_set_p("Alias.*", flags) \
		&& !flag_set_p("Ignore", flags))
}

# If FLAGS describes an option that needs state without a public
# variable name, return the name of that field, minus the initial
# "x_", otherwise return "".  NAME is the name of the option.
function static_var(name, flags)
{
	if (global_state_p(flags) || !needs_state_p(flags))
		return ""
	gsub ("[^" alnum "]", "_", name)
	return "VAR_" name
}

# Return the type of variable that should be associated with the given flags.
function var_type(flags)
{
	if (flag_set_p("Defer", flags))
		return "void *"
	else if (flag_set_p("Enum.*", flags)) {
		en = opt_args("Enum", flags);
		return enum_type[en] " "
	}
	else if (!flag_set_p("Joined.*", flags) && !flag_set_p("Separate", flags))
		return "int "
	else if (flag_set_p("Host_Wide_Int", flags))
		return "HOST_WIDE_INT "
	else if (flag_set_p("UInteger", flags))
		return "int "
	else
		return "const char *"
}

# Return the type of variable that should be associated with the given flags
# for use within a structure.  Simple variables are changed to signed char
# type instead of int to save space.
function var_type_struct(flags)
{
	if (flag_set_p("UInteger", flags)) {
		if (host_wide_int[var_name(flags)] == "yes")
			return "HOST_WIDE_INT ";
		if (flag_set_p("ByteSize", flags))
			return "HOST_WIDE_INT "
	  return "int "
	}
	else if (flag_set_p("Enum.*", flags)) {
		en = opt_args("Enum", flags);
		return enum_type[en] " "
	}
	else if (!flag_set_p("Joined.*", flags) && !flag_set_p("Separate", flags)) {
		if (flag_set_p(".*Mask.*", flags)) {
			if (host_wide_int[var_name(flags)] == "yes")
				return "HOST_WIDE_INT "
			else
				return "/* - */ int "
		}
		else
			return "signed char "
	}
	else
		return "const char *"
}

# Given that an option has flags FLAGS, return an initializer for the
# "var_enum", "var_type" and "var_value" fields of its cl_options[] entry.
function var_set(flags)
{
	if (flag_set_p("Defer", flags))
		return "0, CLVC_DEFER, 0"
	s = nth_arg(1, opt_args("Var", flags))
	if (s != "")
		return "0, CLVC_EQUAL, " s
	s = opt_args("Mask", flags);
	if (s != "") {
		vn = var_name(flags);
		if (vn)
			return "0, CLVC_BIT_SET, OPTION_MASK_" s
		else
			return "0, CLVC_BIT_SET, MASK_" s
	}
	s = nth_arg(0, opt_args("InverseMask", flags));
	if (s != "") {
		vn = var_name(flags);
		if (vn)
			return "0, CLVC_BIT_CLEAR, OPTION_MASK_" s
		else
			return "0, CLVC_BIT_CLEAR, MASK_" s
	}
	if (flag_set_p("Enum.*", flags)) {
		en = opt_args("Enum", flags);
		if (flag_set_p("EnumSet", flags))
			return enum_index[en] ", CLVC_ENUM, CLEV_SET"
		else if (flag_set_p("EnumBitSet", flags))
			return enum_index[en] ", CLVC_ENUM, CLEV_BITSET"
		else
			return enum_index[en] ", CLVC_ENUM, CLEV_NORMAL"
	}
	if (var_type(flags) == "const char *")
		return "0, CLVC_STRING, 0"
	if (flag_set_p("ByteSize", flags))
		return "0, CLVC_SIZE, 0"
	return "0, CLVC_INTEGER, 0"
}

# Given that an option called NAME has flags FLAGS, return an initializer
# for the "flag_var" field of its cl_options[] entry.
function var_ref(name, flags)
{
	name = var_name(flags) static_var(name, flags)
	if (name != "")
		return "offsetof (struct gcc_options, x_" name ")"
	if (opt_args("Mask", flags) != "")
		return "offsetof (struct gcc_options, x_target_flags)"
	if (opt_args("InverseMask", flags) != "")
		return "offsetof (struct gcc_options, x_target_flags)"
	return "(unsigned short) -1"
}

# Given the option called NAME return a sanitized version of its name.
function opt_sanitized_name(name)
{
	gsub ("[^" alnum "]", "_", name)
	return name
}

# Given the option called NAME return the appropriate enum for it.
function opt_enum(name)
{
	return "OPT_" opt_sanitized_name(name)
}

# Given the language called NAME return a sanitized version of its name.
function lang_sanitized_name(name)
{
    gsub( "[^" alnum "_]", "X", name )
    return name
}

# Search for a valid var_name among all OPTS equal to option NAME.
# If not found, return "".
function search_var_name(name, opt_numbers, opts, flags, n_opts)
{
    opt_var_name = var_name(flags[opt_numbers[name]]);
    if (opt_var_name != "") {
        return opt_var_name;
    }
    for (k = 0; k < n_opts; k++) {
        if (opts[k] == name && var_name(flags[k]) != "") {
            return var_name(flags[k]);
        }
    }
    return ""
}

function integer_range_info(range_option, init, option, uinteger_used)
{
    if (range_option != "") {
	ival = init + 0;
	start = nth_arg(0, range_option) + 0;
	end = nth_arg(1, range_option) + 0;
	if (init != "" && init != "-1" && (ival < start || ival > end))
	  print "#error initial value " init " of '" option "' must be in range [" start "," end "]"
	if (uinteger_used && start < 0)
	  print "#error '" option"': negative IntegerRange (" start ", " end ") cannot be combined with UInteger"
	return start ", " end
    }
    else
        return "-1, -1"
}

# Find the index of VAR in VAR_ARRY which as length N_VAR_ARRY.  If
# VAR is not found, return N_VAR_ARRY. That means the var is a new
# defination.
function find_index(var, var_arry, n_var_arry)
{
    for (var_index = 0; var_index < n_var_arry; var_index++)
    {
        if (var_arry[var_index] == var)
            break
    }
    return var_index
}
