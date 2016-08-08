PULSEAUDIO_SRC?=.

LOCALBASE?=	/usr/local

CFLAGS+=	`pkg-config --cflags libpulse`
LDFLAGS+=	`pkg-config --libs libpulse`

CFLAGS+=	-fPIC -DPIC -I.

module-sndio.so: module-sndio.o
	${CC} -shared module-sndio.o \
		-Wl,-rpath -Wl,${LOCALBASE}/lib \
		-Wl,-rpath -Wl,${LOCALBASE}/lib/pulseaudio \
		${LDFLAGS} \
		-L${LOCALBASE}/lib/pulseaudio \
		-lpulsecore-8.0 \
		-lpulsecommon-8.0 \
		-lsndio \
		-o module-sndio.so



clean:
	rm -f module-sndio.o module-sndio.so
