#!/bin/sh
#
#  Copyright (C) 2003 Free Software Foundation, Inc.
#  Contributed by Neil Booth, May 2003.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2, or (at your option) any
# later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
#
# Usage: opts.sh moveifchange srcdir outfile.c outfile.h file1.opt [ ...]

# Always operate in the C locale.
LANG=C
LANGUAGE=C
LC_ALL=C
export LANG LANGUAGE LC_ALL

# Set AWK if environment has not already set it.
AWK=${AWK-awk}

SORT=sort		# Could be /bin/sort or /usr/bin/sort

MOVEIFCHANGE=$1; shift
C_FILE=$1; shift
H_FILE=$1; shift
TMP_C_FILE=tmp-${C_FILE}
TMP_H_FILE=tmp-${H_FILE}

${AWK} '
	# Ignore comments and blank lines
	/^[ \t]*(;|$)/	{ next }
	# Note that RS="" falls foul of gawk 3.1.2 bugs
	/^[^ \t]/       { record = $0
			  do { getline tmp;
			       if (!(tmp ~ "^[ \t]*(;|$)"))
			          record = record "\034" tmp
			  } while (tmp != "")
			  print record
			}
' "$@" | ${SORT} | ${AWK} '
    function switch_flags (flags,   result)
    {
	flags = " " flags " "
	result = "0"
	for (j = 0; j < n_langs; j++) {
	    regex = " " langs[j] " "
	    gsub ( "\\+", "\\+", regex )
	    if (flags ~ regex)
		result = result " | " macros[j]
	}
        if (flags ~ " Common ") result = result " | CL_COMMON"
        if (flags ~ " Joined ") result = result " | CL_JOINED"
        if (flags ~ " JoinedOrMissing ") \
		result = result " | CL_JOINED | CL_MISSING_OK"
        if (flags ~ " Separate ") result = result " | CL_SEPARATE"
        if (flags ~ " RejectNegative ") result = result " | CL_REJECT_NEGATIVE"
        if (flags ~ " UInteger ") result = result " | CL_UINTEGER"
        if (flags ~ " Undocumented ") result = result " | CL_UNDOCUMENTED"
	sub( "^0 \\| ", "", result )
	return result
    }

    BEGIN {
	FS = "\034"
	n_opts = 0
	n_langs = 0
    }

# Collect the text and flags of each option into an array
    {
	if ($1 == "Language") {
		langs[n_langs] = $2
		n_langs++;
	} else {
		opts[n_opts] = $1
		flags[n_opts] = $2
		help[n_opts] = $3
		n_opts++;
	}
    }

# Dump out an enumeration into a .h file, and an array of options into a
# C file.  Combine the flags of duplicate options.
    END {
 	c_file = "'${TMP_C_FILE}'"
 	h_file = "'${TMP_H_FILE}'"
 	realh_file = "'${H_FILE}'"
	comma = ","

	print "/* This file is auto-generated by opts.sh.  */\n" > c_file
	print "#include <intl.h>"			>> c_file
	print "#include \"" realh_file "\""		>> c_file
	print "#include \"opts.h\"\n"			>> c_file
	print "const char * const lang_names[] =\n{"	>> c_file

	print "/* This file is auto-generated by opts.sh.  */\n" > h_file
	for (i = 0; i < n_langs; i++) {
	    macros[i] = "CL_" langs[i]
	    gsub( "[^A-Za-z0-9_]", "X", macros[i] )
	    s = substr("         ", length (macros[i]))
	    print "#define " macros[i] s " (1 << " i ")" >> h_file
	    print "  \"" langs[i] "\","			>> c_file
	}

	print "  0\n};\n"				>> c_file
	print "const unsigned int cl_options_count = N_OPTS;\n" >> c_file
	print "const struct cl_option cl_options[] =\n{" >> c_file

	print "\nenum opt_code\n{"			>> h_file

	for (i = 0; i < n_opts; i++)
	    back_chain[i] = "N_OPTS";

	for (i = 0; i < n_opts; i++) {
	    # Combine the flags of identical switches.  Switches
	    # appear many times if they are handled by many front
	    # ends, for example.
	    while( i + 1 != n_opts && opts[i] == opts[i + 1] ) {
		flags[i + 1] = flags[i] " " flags[i + 1];
		i++;
	    }

	    len = length (opts[i]);
	    enum = "OPT_" opts[i]
	    if (opts[i] == "finline-limit=")
		enum = enum "eq"
	    gsub ("[^A-Za-z0-9]", "_", enum)

	    # If this switch takes joined arguments, back-chain all
	    # subsequent switches to it for which it is a prefix.  If
	    # a later switch S is a longer prefix of a switch T, T
	    # will be back-chained to S in a later iteration of this
	    # for() loop, which is what we want.
	    if (flags[i] ~ "Joined") {
		for (j = i + 1; j < n_opts; j++) {
		    if (substr (opts[j], 1, len) != opts[i])
			break;
		    back_chain[j] = enum;
		}
	    }

	    s = substr("                                  ", length (opts[i]))
	    if (i + 1 == n_opts)
		comma = ""

	    if (help[i] == "")
		hlp = "0"
	    else
	    	hlp = "N_(\"" help[i] "\")";

	    printf("  %s,%s/* -%s */\n", enum, s, opts[i]) >> h_file
	    printf("  { \"-%s\",\n    %s,\n    %s, %u, %s }%s\n",
		   opts[i], hlp, back_chain[i], len,
		   switch_flags(flags[i]), comma)	>> c_file
	}

	print "  N_OPTS\n};"				>> h_file
	print "};"					>> c_file
    }
'

# Copy the newly generated files back to the correct names only if different.
# This is to prevent a cascade of file rebuilds when not necessary.
${MOVEIFCHANGE} ${TMP_H_FILE} ${H_FILE}
${MOVEIFCHANGE} ${TMP_C_FILE} ${C_FILE}
