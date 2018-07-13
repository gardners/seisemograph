all:	continuous-graph

continuous-graph:	continuous-graph.c code_instrumentation.h code_instrumentation.c serial.c
	gcc -Wall -O6 -o continuous-graph continuous-graph.c code_instrumentation.c serial.c -lpng
