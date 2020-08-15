#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<stdbool.h>
#include<assert.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<poll.h>
#include<libgen.h>
#include<sys/epoll.h>
#include<sys/wait.h>
#include<sys/mman.h>
#include<sys/stat.h>
#include<signal.h>

#define USER_LIMIT 5
#define BUF_SIZE 1024
#define FD_LIMIT 65535
#define MAX_EVENT_NUM 1024
#define PROC_LIMIT 65535

struct client_data
{
	struct sockaddr_in addr;
	int connfd;
	pid_t pid;
	int pipefd[2];
	char* cliName;
};

static const char* shm_name="/my_shm";
int sig_pipefd[2];
int epollfd;
int listenfd;
int shmfd;
char* share_mem=0;
//客户连接数组，进程用客户连接的编号来索引这个数组
struct client_data* users=0;
//子进程和客户连接的映射关系表
int* sub_proc=0;
//当前客户数量
int user_count=0;
bool stop_child=false;

int setnonblocking(int fd)
{
	int old_opt=fcntl(fd,F_GETFL);
	int new_opt=old_opt | O_NONBLOCK;
	fcntl(fd,F_SETFL,new_opt);
	return old_opt;
}

void addfd(int epollfd,int fd)
{
	struct epoll_event event;
	event.data.fd=fd;
	event.events=EPOLLIN|EPOLLET;
	epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
	setnonblocking(fd);
}

void sig_handler(int sig)
{
	int save_errno=errno;
	int msg=sig;
	send(sig_pipefd[1],(char*)&msg,1,0);
	errno=save_errno;
}

void addsig(int sig,void(*handler)(int),bool restart=true)
{
	struct sigaction sa;
	memset(&sa,'\0',sizeof(sa));
	sa.sa_handler=handler;
	if(restart)
	{
		sa.sa_flags|=SA_RESTART;
	}
	sigfillset(&sa.sa_mask);
	assert(sigaction(sig,&sa,NULL)!=-1);
}

void del_resourse()
{
	close(sig_pipefd[0]);
	close(sig_pipefd[1]);
	close(listenfd);
	close(epollfd);
	shm_unlink(shm_name);
	delete [] users;
	//for(int i=0;i<user_count;++i)
		//free(users[i]);
	delete [] sub_proc;
	//for(int i=0;i<PROC_LIMIT;++i)
		//free(sub_proc[i]);
}

void child_term_handler(int sig)
{
	stop_child=true;
}

int run_child(int idx,client_data* users,char* share_mem)
{
	struct epoll_event events[MAX_EVENT_NUM];
	int child_epollfd=epoll_create(5);
	assert(child_epollfd!=-1);
	int connfd=users[idx].connfd;
	addfd(child_epollfd,connfd);
	int pipefd=users[idx].pipefd[1];
	addfd(child_epollfd,pipefd);
	int ret;
	addsig(SIGTERM,child_term_handler,false);

	while(!stop_child)
	{
		int num=epoll_wait(child_epollfd,events,MAX_EVENT_NUM,-1);
		if((num<0)&&(errno!=EINTR))
		{
			printf("epoll error\n");
			break;
		}
		for(int i=0;i<num;++i)
		{
			int sockfd=events[i].data.fd;
			if((sockfd==connfd)&&(events[i].events&EPOLLIN))
			{
				memset(share_mem+idx*BUF_SIZE,'\0',BUF_SIZE);
				ret=recv(connfd,share_mem+idx*BUF_SIZE,BUF_SIZE-1,0);
				if(ret<0)
				{
					if(errno!=EAGAIN)
					{
						stop_child=true;
					}
				}
				else if(ret==0)
				{
					stop_child=true;
				}
				else
				{
					send(pipefd,(char*)&idx,sizeof(idx),0);
				}
			}
			else if((sockfd==pipefd)&&(events[i].events&EPOLLIN))
			{
				int client=0;
				ret=recv(sockfd,(char*)&client,sizeof(client),0);
				if(ret<0)
				{
					if(errno!=EAGAIN)
					{
						stop_child=true;
					}
				}
				else if(ret==0)
				{
					stop_child=true;
				}
				else
				{
					send(connfd,share_mem+client*BUF_SIZE,BUF_SIZE,0);
				}

			}
			else
			{
				continue;
			}
		}
	}
	close(connfd);
	close(pipefd);
	close(child_epollfd);
	return 0;
}

int main(int argc,char*argv[])
{
	if(argc<=2)
	{
		printf("usage: %s ip_addr port_num\n",basename(argv[0]));
		return 1;
	}
	const char* ip=argv[1];
	int port=atoi(argv[2]);

	int ret=0;
	struct sockaddr_in addr;
	bzero(&addr,sizeof(addr));
	addr.sin_family=AF_INET;
	inet_pton(AF_INET,ip,&addr.sin_addr);
	addr.sin_port=htons(port);

	int listenfd=socket(PF_INET,SOCK_STREAM,0);
	assert(listenfd>=0);
	ret=bind(listenfd,(struct sockaddr*)&addr,sizeof(addr));
	assert(ret!=-1);
	ret=listen(listenfd,5);
	assert(ret!=-1);
	user_count=0;
	users=new client_data[USER_LIMIT+1];
	//users=(client_data*)malloc((USER_LIMIT+1)*sizeof(struct client_data));
	sub_proc=new int[PROC_LIMIT];
	//sub_proc=(int*)malloc(PROC_LIMIT);
	for(int i=0;i<PROC_LIMIT;++i)
	{
		sub_proc[i]=-1;
	}

	epoll_event events[MAX_EVENT_NUM];
	epollfd=epoll_create(5);
	assert(epollfd!=-1);
	addfd(epollfd,listenfd);

	ret=socketpair(PF_UNIX,SOCK_STREAM,0,sig_pipefd);
	assert(ret!=-1);
	setnonblocking(sig_pipefd[1]);
	addfd(epollfd,sig_pipefd[0]);

	addsig(SIGCHLD,sig_handler);
	addsig(SIGTERM,sig_handler);
	addsig(SIGINT,sig_handler);
	addsig(SIGPIPE,SIG_IGN);
	bool stop_server=false;
	bool terminate=false;

	shmfd=shm_open(shm_name,O_CREAT|O_RDWR,0666);
	assert(shmfd!=-1);
	ret=ftruncate(shmfd,USER_LIMIT*BUF_SIZE);
	assert(ret!=-1);

	share_mem=(char*)mmap(NULL,USER_LIMIT*BUF_SIZE,PROT_READ|PROT_WRITE,MAP_SHARED,shmfd,0);
	assert(share_mem!=MAP_FAILED);
	close(shmfd);

	while(!stop_server)
	{
		int num=epoll_wait(epollfd,events,MAX_EVENT_NUM,-1);
		if((num<0)&&(errno!=EINTR))
		{
			printf("epoll failure\n");
			break;
		}

		for(int i=0;i<num;++i)
		{
			int sockfd=events[i].data.fd;
			if(sockfd==listenfd)
			{
				struct sockaddr_in cli_addr;
				socklen_t cli_addr_len=sizeof(cli_addr);
				int connfd=accept(listenfd,(struct sockaddr*)&cli_addr,&cli_addr_len);

				if(connfd<0)
				{
					printf("accept error\n");
					continue;
				}
				if(user_count>=USER_LIMIT)
				{
					const char* info="too many users\n";
					printf("%s\n",info);
					send(connfd,info,strlen(info),0);
					close(connfd);
					continue;
				}

				users[user_count].addr=cli_addr;
				users[user_count].connfd=connfd;

				ret=socketpair(PF_UNIX,SOCK_STREAM,0,users[user_count].pipefd);
				assert(ret!=-1);
				pid_t pid=fork();
				if(pid<0)
				{
					close(connfd);
					continue;
				}
				else if(pid==0)
				{
					close(epollfd);
					close(listenfd);
					close(users[user_count].pipefd[0]);
					close(sig_pipefd[0]);
					close(sig_pipefd[1]);
					run_child(user_count,users,share_mem);
					munmap((void*)share_mem,USER_LIMIT*BUF_SIZE);
					exit(0);
				}
				else
				{
					close(connfd);
					close(users[user_count].pipefd[1]);
					addfd(epollfd,users[user_count].pipefd[0]);
					users[user_count].pid=pid;

					sub_proc[pid]=user_count;
					++user_count;
				}
			}
			else if((sockfd==sig_pipefd[0])&&(events[i].events&EPOLLIN))
			{
				int sig;
				char signals[1024];
				ret=recv(sig_pipefd[0],signals,sizeof(signals),0);
				if(ret==-1)
				{
					continue;
				}
				else if(ret==0)
				{
					continue;
				}
				else
				{
					for(int i=0;i<ret;++i)
					{
						switch(signals[i])
						{
							case SIGCHLD:
							{
								pid_t pid;
								int stat;
								while((pid=waitpid(-1,&stat,WNOHANG))>0)
								{
									int del_user=sub_proc[pid];
									sub_proc[pid]=-1;
									if((del_user<0)||(del_user>USER_LIMIT))
									{
										continue;
									}

									epoll_ctl(epollfd,EPOLL_CTL_DEL,users[del_user].pipefd[0],0);
									close(users[del_user].pipefd[0]);
									users[del_user]=users[--user_count];
									sub_proc[users[del_user].pid]=del_user;
								}
								if(terminate&&user_count==0)
								{
									stop_server=true;
								}
								break;
							}
							case SIGTERM:
							case SIGINT:
							{
								printf("kill all the child now\n");
								if(user_count==0)
								{
									stop_server=true;
									break;
								}
								for(int i=0;i<user_count;++i)
								{
									int pid=users[i].pid;
									kill(pid,SIGTERM);
								}
								terminate=true;
								break;
							}
							default:
							{
								break;
							}
						}
					}
				}
			}
			else if(events[i].events&EPOLLIN)
			{
				int child=0;
				ret=recv(sockfd,(char*)&child,sizeof(child),0);
				printf("read data from child across pipe\n");
				if(ret==-1)
				{
					continue;
				}
				else if(ret==0)
				{
					continue;
				}
				else
				{
					for(int j=0;j<user_count;++j)
					{
						if(users[j].pipefd[0]!=sockfd)
						{
							printf("send data to child across pipe\n");
							send(users[j].pipefd[0],(char*)&child,sizeof(child),0);
						}
					}
				}

			}
		}
	}

	del_resourse();
	return 0;
}











