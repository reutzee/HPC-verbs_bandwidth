all: client server
client:bw_template.c
	ln -s client server
server: bw_template.c
	gcc bw_template.c -libverbs -o server
clean:
	rm -f *.o client server 
