#include <errno.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <resolv.h>
#include <thread>
#include <mutex>
#include <vector>
#include "openssl/ssl.h"
#include "openssl/err.h"
#define FAIL    -1
using namespace std;

vector<SSL*> childfd;
mutex m;

// Create the SSL socket and intialize the socket address structure
int OpenListener(int port)
{
    int sd;
    struct sockaddr_in addr;
    sd = socket(PF_INET, SOCK_STREAM, 0);
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0 )
    {
        perror("can't bind port");
        abort();
    }
    if ( listen(sd, 10) != 0 )
    {
        perror("Can't configure listening port");
        abort();
    }
    return sd;
}
int isRoot()
{
    if (getuid() != 0)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}
SSL_CTX* InitServerCTX(void)
{
    SSL_METHOD *method;
    SSL_CTX *ctx;
    OpenSSL_add_all_algorithms();  /* load & register all cryptos, etc. */
    SSL_load_error_strings();   /* load all error messages */
    method = (ssl_method_st*)TLSv1_2_server_method();  /* create new server-method instance */
    ctx = SSL_CTX_new(method);   /* create new context from method */
    if ( ctx == NULL )
    {
        ERR_print_errors_fp(stderr);
        abort();
    }
    return ctx;
}
void LoadCertificates(SSL_CTX* ctx, char* CertFile, char* KeyFile)
{
    /* set the local certificate from CertFile */
    if ( SSL_CTX_use_certificate_file(ctx, CertFile, SSL_FILETYPE_PEM) <= 0 )
    {
        ERR_print_errors_fp(stderr);
        abort();
    }
    /* set the private key from KeyFile (may be the same as CertFile) */
    if ( SSL_CTX_use_PrivateKey_file(ctx, KeyFile, SSL_FILETYPE_PEM) <= 0 )
    {
        ERR_print_errors_fp(stderr);
        abort();
    }
    /* verify private key */
    if ( !SSL_CTX_check_private_key(ctx) )
    {
        fprintf(stderr, "Private key does not match the public certificate\n");
        abort();
    }
}
void ShowCerts(SSL* ssl)
{
    X509 *cert;
    char *line;
    cert = SSL_get_peer_certificate(ssl); /* Get certificates (if available) */
    if ( cert != NULL )
    {
        printf("Server certificates:\n");
        line = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
        printf("Subject: %s\n", line);
        free(line);
        line = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
        printf("Issuer: %s\n", line);
        free(line);
        X509_free(cert);
    }
    else
        printf("No certificates.\n");
}
void Servlet(SSL* ssl, bool broadcast, struct sockaddr_in addr) /* Serve the connection -- threadable */
{
    char buf[1024] = {0};
    int sd, bytes;
    if ( SSL_accept(ssl) == FAIL ) {    /* do SSL-protocol accept */
        ERR_print_errors_fp(stderr);
	return;
    }
    
    while (true) {
        bytes = SSL_read(ssl, buf, sizeof(buf)); /* get request */
	if(bytes <= 0) 
		break;
        buf[bytes] = '\0';
        printf("Client with port %d msg: %s\n", ntohs(addr.sin_port), buf);
        if (broadcast)
        {
	    m.lock();
	    for(vector<SSL*>::iterator it = childfd.begin(); it != childfd.end(); it++) {
		    SSL_write(*it, buf, strlen(buf));
	    }
	    m.unlock();
        }
	else {
	    SSL_write(ssl, buf, strlen(buf));
	}
    }
    m.lock();
    for(vector<SSL*>::iterator it = childfd.begin(); it != childfd.end(); ) {
	    if(*it == ssl) {
		    printf("Disconnection: %s:%d\n",inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
		    childfd.erase(it);
	    }
	    else
		    it++;
    }
    m.unlock();
    sd = SSL_get_fd(ssl);       /* get socket connection */
    SSL_free(ssl);         /* release SSL state */
    close(sd);          /* close connection */
    return;
}
int main(int count, char *Argc[])
{
    SSL_CTX *ctx;
    int server;
    char *portnum;
    bool broadcast = false;
//Only root user have the permsion to run the server
    if(!isRoot())
    {
        printf("This program must be run as root/sudo user!!\n");
        exit(0);
    }
    if ( count < 2 && count > 3 )
    {
        printf("syntax: echo_server <port> [-b]\n");
        printf("example: echo_server 1234 [-b]\n");
        printf("option [-b]: send message to every clients after receive message\n");

        exit(0);
    }
    if(count == 3) {
	    if(!strcmp(Argc[2], "-b")) {
		    broadcast = true;
		    printf("broadcast mode on\n");
	    }
	    else {
		    printf("invalid option\n");
		    printf("option [-b]: send message to every clients after receive message\n");
		    return -1;
	    }
    }

    // Initialize the SSL library
    SSL_library_init();
    portnum = Argc[1];
    ctx = InitServerCTX();        /* initialize SSL */
    LoadCertificates(ctx, "mycert.pem", "mycert.pem"); /* load certs */
    server = OpenListener(atoi(portnum));    /* create server socket */
    printf("Complete server setting and waiting for connection\n");
    struct sockaddr_in addr;
    while (1)
    {
        socklen_t len = sizeof(addr);
        SSL *ssl;
        int client = accept(server, (struct sockaddr*)&addr, &len);  /* accept connection as usual */

        printf("Connection: %s:%d\n",inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        ssl = SSL_new(ctx);              /* get new SSL state with context */
        SSL_set_fd(ssl, client);      /* set connection socket to SSL state */

	m.lock();
	childfd.push_back(ssl);
	m.unlock();
	
	thread t(Servlet, ssl, broadcast, addr);
        t.detach();         /* service connection */
    }
    close(server);          /* close server socket */
    SSL_CTX_free(ctx);         /* release context */
}
