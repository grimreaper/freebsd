#	$Id: dot.login,v 1.6 1994/09/16 04:20:13 rgrimes Exp $
#
tset -Q \?$TERM
stty crt erase ^H
umask 2
# plain csh is too stupid to get any information on ARGV[0] back
if (! $?tcsh) then
  echo "Don't login as root, login as yourself and use the 'su' command"
else
  # for tcsh, check if we have been invoked by an "su -"
  if ("$0" != "-su") \
    echo "Don't login as root, login as yourself and use the 'su' command"
endif

