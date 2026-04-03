run: pipeline
	./pipeline
pipeline: ./src/*.c
	gcc ./src/solution1.c ./src/helpers.c -I ./include -o pipeline