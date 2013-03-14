/*			proxy.c - A Mutithreaded Caching Web Proxy
 *	Team member 1: Vignesh Gadiyar, Andrew ID: vgadiyar
 *	Team member 2: Pradeep Kumar Vikraman, Andrew ID: pvikrama
 *
 *	Description: The Proxy developed uses HTTP/1.0 queries to query the server.
 *	Multi-threading is employed by using the pthread functions and caching is 
 *	done by using a linked list structure whose node looks as follows:
 *			 +---------------------------------------------------+
 *			 +  Url, lru, data, size, next pointer, prev pointer +
 *			 +---------------------------------------------------+
 *	The checking is done by using the URL. If we get a hit, we serve the
 *	data present in the node to the client. Else, we check whether the incoming
 *	size of the object is greater than MAX_OBJECT_SIZE. If yes, we don't cache 
 *	object. If no, we cache that object and increment its lru bit. This lru bit
 *	acts as a timestamp value during eviction. This bit is also incremented when
 *	we touch a particular node. Merge Sort function is used to sort the list 
 *	during the eviction process
*/

#include "csapp.h"

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

static const char *user_agent = "User-Agent: Mozilla/5.0 (X11; Linux x86_64;\
								rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *accept1 = "Accept: text/html,application/xhtml+xml,\
							  application/xml;q=0.9,*/*;q=0.8\r\n";
static const char *accept_encoding = "Accept-Encoding: gzip, deflate\r\n";
static const char *connection = "Connection: close\r\n";
static const char *proxyconn = "Proxy-Connection: close\r\n";

/* Structure for storing modified client request */
struct student
{
	char host[MAXLINE];
	char buf1[MAXLINE];
	char *url;
	int nomsg;
	int portno;
};
typedef struct student Student;

/* Structure for Cache */
struct node
{
	int lru;
	int size;
	char *data;
	char *url;
	struct node * next;
	struct node * prev;
};
typedef struct node Node;
/* Head of linked list to store cache */
Node *head = NULL;

/* Function declarations for cache functions */
void Merge(Node **array, int left, int mid, int right);
void cache_add(char *rp, int object_size, char *url);
void eviction(int object_size);
void MergeSort(Node **array, int left, int right);
int lookup(char *url, int connfd);
void serve_client(int connfd, char *usrbuf, int object_size);
void delete(struct node *delete);

/* Function declarations for concurrent proxy */
Student *parse(int);
void our_writen(int fd, void *usrbuf, size_t n);
ssize_t our_readnb(rio_t *rp, void *usrbuf, size_t n);
void *thread(void *vargp);
handler_t *mySignal(int signum, handler_t *handler);
int Our_clientfd(char *hostname, int port);
int our_clientfd(char *hostname, int port) ;

/* Global Variables */
int total_objects=0;
int count=1;
int current_cache_size=0;

/* Initialise the lock variables */
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;

int main(int argc, char **argv) 
{
	int listenfd, port;
	int *connfd;
	socklen_t clientlen;
	struct sockaddr_in clientaddr;
	pthread_t tid;
	/* Ignore SIGPIPE signals */	
	mySignal(SIGPIPE, SIG_IGN);

	/* Check command line args */
	if (argc != 2) 
	{
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(1);
	}
	port = atoi(argv[1]);
	
	/* Creating Proxy's listening port */
	listenfd = Open_listenfd(port);
	
	while (1) 
	{
		clientlen = sizeof(clientaddr);
		/* Malloc is used to dynamically allocate memory 
		 * to avoid thread's sharing connfd*/
		connfd = (int *) malloc(sizeof(int));
		if(connfd == NULL)
		{
			printf("Out of memory\n");
			return -1;
		}
		*connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
		/* Create new threads to handle different requests */
		Pthread_create(&tid, NULL, thread, connfd);
	}
}

/* Thread function to serve each client request. This function first calls parse
 * to check whether the request is in the cache, else it creates the modified 
 * request, serves the client and then adds it into the cache */
void *thread(void *vargp)
{
		char buf3[MAXLINE];
		rio_t rio;
		int pclientfd, connfd;
		long int sum=0;
		
		connfd = *((int *)vargp);
		Student *check;
		
		/* Thread is detached to free its memory automatically */
		Pthread_detach(pthread_self());
		Free(vargp);
		check = parse(connfd);

		/* If condition entered on cache hit */
		if( check->nomsg == 0)
		{
			close(connfd);
			free(check);
			return NULL;
		}
		
		/* The connection is closed on DNS error */
		pclientfd = Our_clientfd(check->host , check->portno);
		if( pclientfd == -1)
		{
			close(connfd);
			free(check->url);
			free(check);
			return NULL;
		}

		Rio_readinitb(&rio, pclientfd);
		/* Write the modified request message into pclientfd */
		our_writen(pclientfd, check->buf1, strlen(check->buf1));

		/* Write the server's repsonse into a temporary buffer 'cachebuf' 
		 * before updating this into cache */
		char cachebuf[MAX_OBJECT_SIZE];
		int c_size=0;
		/* Copy response from server into temporary buffer */
		while((sum= our_readnb(&rio, buf3, sizeof(buf3))))
		{
			if(c_size+sum <= MAX_OBJECT_SIZE)
			{
				memcpy(cachebuf+c_size, buf3, sum);
			}
			c_size+=sum;
			our_writen(connfd, buf3, sizeof(buf3));
		}
		 
		/* Update cache only if sizeof objects <= MAX_OBJECT_SIZE */
		if(c_size <= MAX_OBJECT_SIZE)
			cache_add(cachebuf, c_size, check->url);
		else
			free(check->url);
		
		free(check);
		Close(pclientfd);
		Close(connfd);
		return NULL;
}

/* Parse function basically does 2 operations, firstly it checks whether the 
 * request is in the cache and if yes serve it. Else it creates the modified
 * request and queries the server and serves the client and caches the 
 * response */
Student *parse(int connfd)
{
	mySignal(SIGPIPE, SIG_IGN);
	rio_t rio;
	Student *str;
	char buf[MAXLINE], method[MAXLINE]="A", version[MAXLINE],\
		hdr[MAXLINE], hdrval[100], uri[MAXLINE];
	int urlsize;
	char *url;
	
	str = (Student *) malloc(sizeof(Student));
	if(str == NULL)
	{
		printf("Out of memory\n");
		return NULL;
	}
	Rio_readinitb(&rio, connfd);
	Rio_readlineb(&rio, buf, MAXLINE);
	sscanf(buf, "%s %s %s", method, uri, version);
	
	/* Dynamically allocate memory for url to prevent sharing between threads */
	urlsize = sizeof(uri);
	url = (char *)malloc(urlsize);
	if(url == NULL)
	{
		printf("Out of memory\n");
		return NULL;
	}
	strcpy(url,uri);
	
	/* Handle only GET requests */
	if(strcmp(method,"GET")==0)
	{
		char *hostsrt,*pathsrt;
		char *port, buftmp[20], temp1[300];
		int portno = 80;
		
		int ret;
		/* Call cache lookup */
		ret = lookup(url, connfd);
		/*Cache Hit */
		if(ret == 1)
		{
			str->nomsg=0;
			free(url);
			return str;
		}

		str->url = url;
		/* Modifying the version to be 1.0 */
		strcpy(version,"HTTP/1.0");
		hostsrt = uri + 7;
		/* Get Host name */
		 if(!strstr(hostsrt, ":"))
			sscanf(hostsrt, "%[^/]s", str->host);
		else
		{
			sscanf(hostsrt, "%[^:]s", temp1);
			if( strstr(temp1, "/"))
				sscanf(temp1, "%[^/]s", str->host);
			else
				strcpy(str->host, temp1);
		}

		port = strstr(hostsrt, ":");
		pathsrt = strstr(hostsrt , "/");
		/* Get URI */
		if(pathsrt)
			sscanf(pathsrt, "%s", uri);
		else
			strcpy(uri,"/");

		/* Populating the port variable with the value in request message
		 * if its present or with a default value of 80 if nothing is present
		 */
		if(port != NULL)
		{
			sscanf(port+1, "%[^/]s", buftmp); 
			portno = atoi(buftmp);
		}
		else
			portno=80;
		if(portno == 0)
			portno = 80;
		str->portno = portno;
		
		/* Populating buf1 which contains modified HTTP 1.0 request */
		sprintf(str->buf1, "%s %s %s\r\n", method, uri, version);
		sprintf(str->buf1, "%sHost: %s\r\n", str->buf1, str->host);
		sprintf(str->buf1, "%s%s" , str->buf1, user_agent);
		sprintf(str->buf1, "%s%s" , str->buf1, accept1);
		sprintf(str->buf1, "%s%s" , str->buf1, accept_encoding);
		sprintf(str->buf1, "%s%s" , str->buf1, connection);
		sprintf(str->buf1, "%s%s" , str->buf1, proxyconn);
		
		Rio_readlineb(&rio, buf, MAXLINE);
		
		/* Populate BUF1 with the same header, header values as the original
         * request for all other header's apart from the hardcored ones 
		 * and do this until you reach a blank line (\r\n) 
		 */
		while(strcmp(buf, "\r\n")) 
		{
			sscanf(buf,"%s %s", hdr, hdrval);

			if (strcmp(hdr, "Host:")!=0 && strcmp(hdr, "User-Agent:")!=0 && \
 			strcmp(hdr, "Accept:")!=0 && strcmp(hdr, "Accept-Encoding:")!=0 &&\
			strcmp( hdr,"Connection:")!=0 && strcmp(hdr,"Proxy-Connection:")!=0)
				sprintf(str->buf1, "%s%s", str->buf1, buf);
			Rio_readlineb(&rio, buf, MAXLINE);
		}

		sprintf(str->buf1, "%s\r\n", str->buf1);

		str->nomsg = 1;
		return str;
	}
	
	/* Ignoring Non-GET requests */
	else
	{
		free(url);
		str->nomsg = 0;
		return str;
	}
}

/* Wrapper for rio_readnb to not exit on error */
ssize_t our_readnb(rio_t *rp, void *usrbuf, size_t n) 
{
    ssize_t rc;

    if ((rc = rio_readnb(rp, usrbuf, n)) < 0)
	{
		printf("Rio_readnb error\n");
		return 0;
	}
    return rc;
}

/* Wrapper for rio_writen to not exit on error */
void our_writen(int fd, void *usrbuf, size_t n) 
{
    if (rio_writen(fd, usrbuf, n) != n)
		return;
}

/* Lookup function to know whether there is a Cache hit or miss */
int lookup(char *url, int connfd)
{
	/* Using a read lock for the lookup function so as to allow many readers
	 * to lookup together, while at the same time blocking all the writers 
	 * until the readers currently present are done executing the read lock
	 */
	pthread_rwlock_rdlock(&rwlock);
	Node *n;
	for( n = head; n != NULL; n = n->next)
	{
		/* If an entry already exists in the cache */
		if(strcmp(n->url, url) == 0)
		{
			/* Using a mutex to avoid race conditions among reader threads by 
			 * not letting them simultaneously modify the count variable
			 */
			pthread_mutex_lock(&mutex);
			n->lru = count;
			count=count+1;
			pthread_mutex_unlock(&mutex);
			
			/*Copy data from cache into a temp buffer before unlocking read
			 *this is done because copying into temporary buffer is faster
			 *than serving the data all over the network to the client, hence
			 *helping us spend less time inside readlock
			 */
			char tempbuf[n->size];
			memcpy(tempbuf, n->data, n->size);
			
			pthread_rwlock_unlock(&rwlock);
			/*Copy data from temporary buffer to client after unlocking write*/ 
			serve_client(connfd, tempbuf, n->size);
			return 1;
		}
	}
	pthread_rwlock_unlock(&rwlock);
	return 0;
}

/*
 * Serve_client: This is used to send data from the temporary buffer populated
 * in the lookup function, to the client
 */  
void serve_client(int connfd, char *usrbuf, int object_size)
{
	rio_writen(connfd, usrbuf, object_size);
}

/* Add the new element into the cache */
void cache_add(char *rp, int object_size, char *url)
{
	struct node *new_node;
	/* Using a write lock for the add, delete and eviction function
	 * so as to ensure that there is only one thread doing these operations
	 * and also to ensure reader threads do not execute until the current and
	 * blocked writer threads run to completion
	 */
	pthread_rwlock_wrlock(&rwlock);
	
	/* Call eviction if size of data in cache is going to overflow */
	if(current_cache_size + object_size > MAX_CACHE_SIZE)
	{
		eviction(object_size);
	}

	/* Populate the new cache object */
	new_node = (Node *)malloc(sizeof(Node));
	if(new_node == NULL)
	{
		printf("Out of memory\n");
		return;
	}
	new_node->url = url;
	new_node->size = object_size;
	new_node->lru = count;
	new_node->data = (char *)malloc(object_size);
	if(new_node->data == NULL)
	{
		printf("Out of memory\n");
		return;
	}
	
	/* Copy data into the newly created cache object from the temporary buffer
	 * cachebuf which is populated in thread function and contains server 
	 * response*/
	memcpy(new_node->data, rp, object_size);
	
	/* Add new node to the front of the doubly linked list */
	
	/* If doubly linked list is empty */
	if(head == NULL)
	{
		new_node->next = NULL;
		new_node->prev = NULL;
		head = new_node;
	}
	else
	{
		new_node->next = head;
		new_node->prev = NULL;
	    head->prev = new_node;
		head = new_node;
	}
	
	/* Update Global Variables */
	current_cache_size+=object_size;
	count=count+1;
	total_objects=total_objects+1;
	
	/* Release write lock */
	pthread_rwlock_unlock(&rwlock);
}	

/* Eviction function to evict certain nodes from the cache when the cache is
 * full */
void eviction(int object_size)	
{

	/* Search to find number of nodes to be deleted */
	int i=0, sum=0;
	Node *n[total_objects];
	Node *temp = head;
	/* Populate the node array for sorting */
	while(temp != NULL)
	{
		n[i] = temp;
		temp = temp->next;
		i++;
	}
	/* Sort the array using merge sort */
	MergeSort(n, 0, total_objects-1);

	/* Decide the elements to delete */
	for(i=0; i< total_objects && object_size > 0; i++)
	{
		sum+= n[i]->size;
		delete(n[i]);
		if(sum >= object_size)
		{
			break;
		}
	}
}

/* Function to sort the array based on merge sort */
/* Common mergesort algorithm employed here */
void MergeSort(Node **array, int left, int right)
{
        int mid;
		mid= (left+right)/2;
        /* Sort only when left<right */
        if (left < right)
        {
                /* Sort from left to mid */
                MergeSort(array,left,mid);
                /* Sort from mid to right */
                MergeSort(array,mid+1,right);
                /* Merge operation takes place here*/
                Merge(array,left,mid,right);
        }
}

/* Merge functions to merge 2 arrays. Node array passed to the function
 */
void Merge(Node **array, int left, int mid, int right)
{
        int pos=0,lpos = left,rpos = mid + 1, i;
		Node *tempArray[right-left+1];
		/* Choose where the elemnt has to go based on value */
        while(lpos <= mid && rpos <= right)
        {
				/* Sorting based on lru of the Node */
                if(array[lpos]->lru < array[rpos]->lru)
                {
                        tempArray[pos++] = array[lpos++];
                }
                else
                {
                        tempArray[pos++] = array[rpos++];
                }
        }
        while(lpos <= mid)  tempArray[pos++] = array[lpos++];
        while(rpos <= right) tempArray[pos++] = array[rpos++];
        /* Copy into the original array */
        for(i = 0;i < pos; i++)
        {
                array[i+left] = tempArray[i];
        }
        return;
}
		
/* Delete function to delete a particular node from the cache */
void delete(struct node *delete)
{   
        int size;
		/* Check for boundary conditions, adjust pointers and free memory */
		if(delete->prev != NULL)
			delete->prev->next=delete->next;
	    if(delete->next != NULL)
			delete->next->prev=delete->prev;
		size = delete->size;
        
		free(delete->data);
		free(delete->url);
		free(delete);
		
		/* Update global variables */
        total_objects=total_objects-1;
        current_cache_size -= size;
}

/* Wrapper function for signal */
handler_t *mySignal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
		printf("Signal error");
    return (old_action.sa_handler);
}

/* Threadsafe Open Clientfd */
int our_clientfd(char *hostname, int port) 
{
    int clientfd;
    struct hostent *hp;
    struct sockaddr_in serveraddr;
	int herr, hres;
	char *tmp=NULL;
	int tmplen = MAXLINE;
	struct hostent hostbuf;

    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1; /* check errno for cause of error */

    /* Fill in the server's IP address and port */
	tmp = (char *) malloc(tmplen);
	/* Use threadsafe gethostbyname_r */
	hres = gethostbyname_r(hostname, &hostbuf, tmp, tmplen, &hp, &herr);
	if (hres != 0 || hp == NULL)
	{
		printf("Couldn't resolve name\n");
		return -2;
	}
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)hp->h_addr_list[0], 
	  (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);

    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
		return -1;
    return clientfd;
}

/* Wrapper for Open_clientfd */
int Our_clientfd(char *hostname, int port) 
{
    int rc;
    if ((rc = our_clientfd(hostname, port)) < 0) {
		if (rc == -1)
		{
			printf("Open_clientfd Unix error");
			return -1;
		}
		else  
			return -1;
    }
    return rc;
}
