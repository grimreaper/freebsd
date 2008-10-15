# $FreeBSD$

echo 1..78

TEST1=3222243           # 1970-02-07 07:04:03
TEST2=1005600000        # 2001-11-12 21:11:12
export LANG=C
export TZ=UTC

REGRESSION_START($1)

check()
{
	S=$1
	S_lc=$(echo ${S} | tr A-Z a-z)

	if [ "${S}" = "${S_lc}" ]; then
		FN="_${S}"
	else
		FN="${S}_"
	fi

	REGRESSION_TEST(`1.${FN}', `date -r ${TEST1} +%${S}')
	REGRESSION_TEST(`2.${FN}', `date -r ${TEST2} +%${S}')
}

check A # Saturday Monday
check a # Sat Mon
check B # February November
check b # Feb Nov
check C # 19 20
check c # "Sat Feb  7 07:04:03 1970" "Mon Nov 12 21:20:00 2001"
check D # 02/07/70 11/12/01
check d # 07 12
check e # " 7" 12
check F # "1970-02-07" "2001-11-12"
check G # 1970 2001
check g # 70 01
check H # 07 21
check h # Feb Nov
check I # 07 09
check j # 038 316
check k # " 7" 21
check l # " 7" " 9"
check M # 04 20
check m # 02 11
check p # AM PM
check R # 07:04 21:20
check r # "07:04:03 AM" "09:20:00 PM"
check S # 03 00
check s # ${TEST1} ${TEST2}
check U # 05 45
check u # 6 1
check V # 06 46
check v # " 7-Feb-1970" "12-Nov-2001"
check W # 05 46
check w # 6 1
check X # "07:04:03" "21:20:00"
check x # "02/07/70" "11/12/01"
check Y # 1970 2001
check y # 70 01
check Z # UTC UTC
check z # +0000 +0000
check % # % %
check + # "Sat Feb  7 07:04:03 UTC 1970" "Mon Nov 12 21:20:00 UTC 2001"

REGRESSION_END()
