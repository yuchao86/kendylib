all:
	gcc -g -O3 -c *.c -I../
	ar -rc netframe.a *.o
	cp netframe.a ../lib
	rm -f *.o
debug:
	gcc -g -c *.c -I../ -D _DEBUG -Wall
	ar -rc netframe.a *.o
	rm -f *.o

