#define _GNU_SOURCE 1
#include<sys/types.h>
#include<sys/socket.h>
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<poll.h>
#include<libgen.h>

#define BUF_SIZE 64

int main(int argc,char*argv[])
{
	if(argc<=2)
	{
		printf("usage: %s ip_addr port_num\n",basename(argv[0]));
		return 1;
	}
	const char* ip=argv[1];
	int port=atoi(argv[2]);

	struct sockaddr_in serv_addr;
	bzero(&serv_addr,sizeof(serv_addr));
	serv_addr.sin_family=AF_INET;
	inet_pton(AF_INET,ip,&serv_addr.sin_addr);
	serv_addr.sin_port=htons(port);

	int sockfd=socket(PF_INET,SOCK_STREAM,0);
	assert(sockfd!=-1);
	if(connect(sockfd,(struct sockaddr*)&serv_addr,sizeof(serv_addr))<0)
	{
		printf("connect fail\n");
		close(sockfd);
		return -1;
	}
	struct pollfd fds[2];
	fds[0].fd=0;
	fds[0].events=POLLIN;
	fds[0].revents=0;
	fds[1].fd=sockfd;
	fds[1].events=POLLIN|POLLRDHUP;
	fds[1].revents=0;
	char read_buf[BUF_SIZE];
	int pipefd[2];
	int ret=pipe(pipefd);
	assert(ret!=-1);

	while(1)
	{
		ret=poll(fds,2,-1);
		if(ret<0)
		{
			printf("poll error\n");
			break;
		}
		if(fds[1].revents & POLLRDHUP)
		{
			printf("server close the connection\n");
			break;
		}
		else if(fds[1].revents & POLLIN)
		{
			memset(read_buf,'\0',sizeof(read_buf));
			recv(fds[1].fd,read_buf,BUF_SIZE-1,0);
			printf("others : %s\n",read_buf);
		}
		if(fds[0].revents & POLLIN)
		{
			ret=splice(0,NULL,pipefd[1],NULL,32768,SPLICE_F_MORE | SPLICE_F_MOVE);

			ret=splice(pipefd[0],NULL,sockfd,NULL,32768,SPLICE_F_MORE | SPLICE_F_MOVE);
		}	
	}
	close(sockfd);
	return 0;

}
