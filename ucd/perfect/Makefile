include ../MCONFIG

.c.o:
	$(HOST_CC) $(HOST_CFLAGS) -c $<

O = lookupa.o recycle.o perfhex.o perfect.o

all : perfect

perfect : $(O)
	$(HOST_CC) $(HOST_LDFLAGS) -o perfect $(O) $(HOST_LIBS)

# DEPENDENCIES

lookupa.o : lookupa.c standard.h lookupa.h

recycle.o : recycle.c standard.h recycle.h

perfhex.o : perfhex.c standard.h lookupa.h recycle.h perfect.h

perfect.o : perfect.c standard.h lookupa.h recycle.h perfect.h

clean:
	rm -f *.o perfect
