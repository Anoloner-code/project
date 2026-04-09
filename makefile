run: pipeline
	./pipeline
pipeline: ./src/*.c
	gcc ./src/solution1.c ./src/helpers.c -I ./include -pthread -o pipeline
pipeline2: ./src/*.c
	gcc ./src/solution2.c ./src/helpers.c -I ./include -pthread -o pipeline2
pipeline3: ./src/*.c
	gcc ./src/solution3.c -I ./include -o pipeline3