# $FreeBSD: src/share/mk/bsd.symver.mk,v 1.5 2007/10/18 15:21:35 yar Exp $

.if !target(__<bsd.symver.mk>__)
__<bsd.symver.mk>__:

# Generate the version map given the version definitions
# and symbol maps.
.if !empty(VERSION_DEF) && !empty(SYMBOL_MAPS)
# Find the awk script that generates the version map.
VERSION_GEN?=	version_gen.awk
VERSION_MAP?=	Version.map

# Compute the make's -m path.
_mpath=
_oarg=
.for _arg in ${.MAKEFLAGS}
.if ${_oarg} == "-m"
_mpath+= ${_arg}
.endif
_oarg=  ${_arg}
.endfor
_mpath+= /usr/share/mk

# Look up ${VERSION_GEN} in ${_mpath}.
_vgen=
.for path in ${_mpath}
.if empty(_vgen)
.if exists(${path}/${VERSION_GEN})
_vgen=  ${path}/${VERSION_GEN}
.endif
.endif
.endfor
.if empty(_vgen)
.error ${VERSION_GEN} not found in the search path.
.endif

# Run the symbol maps through the C preprocessor before passing
# them to the symbol version generator.
${VERSION_MAP}: ${VERSION_DEF} ${_vgen} ${SYMBOL_MAPS}
	cat ${SYMBOL_MAPS} | ${CPP} - - \
	    | awk -v vfile=${VERSION_DEF} -f ${_vgen} > ${.TARGET}
.endif	# !empty(VERSION_DEF) && !empty(SYMBOL_MAPS)
.endif  # !target(__<bsd.symver.mk>__)
