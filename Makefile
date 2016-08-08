MKDIR?=	mkdir -p
INSTALL_LIB?=	install -s -m 444
STAGEDIR?=
LOCALBASE?=	/usr/local
PULSE_VERSION?=	8.0
PULSE_MODDIR?=	${LOCALBASE}/lib/pulse-${PULSE_VERSION}/modules
CFLAGS+=	`pkg-config --cflags libpulse`
CFLAGS+=	-fPIC -I.

all: module-sndio.so

module-sndio.so: module-sndio.o
	${CC} -shared module-sndio.o \
		-Wl,-rpath -Wl,${LOCALBASE}/lib \
		-Wl,-rpath -Wl,${LOCALBASE}/lib/pulseaudio \
		${LDFLAGS} \
		-L${LOCALBASE}/lib \
		-L${LOCALBASE}/lib/pulseaudio \
		-lpulsecore-${PULSE_VERSION} \
		-lpulsecommon-${PULSE_VERSION} \
		-lpulse \
		-lsndio \
		-o module-sndio.so

install: all
	${MKDIR} ${STAGEDIR}${PULSE_MODDIR}
	${INSTALL_LIB} module-sndio.so ${STAGEDIR}${PULSE_MODDIR}

clean:
	rm -f module-sndio.o module-sndio.so
