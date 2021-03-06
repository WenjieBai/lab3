
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <memory.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <getopt.h>
#include <sys/ioctl.h>

#include <openssl/rsa.h>       /* SSLeay stuff */
#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "openssl/sha.h"

#include <shadow.h>
#include <crypt.h>


#define HOME "./"
#define CERTF  HOME "server.crt"
#define KEYF  HOME  "server.key"
#define CACERT HOME "ca.crt"

#define HASHLEN 32
#define SALTLEN 5

#define PERROR(x) do { perror(x); exit(1); } while (0)
#define ERROR(x, args ...) do { fprintf(stderr,"ERROR:" x, ## args); exit(1); } while (0)

#define ENC 1
#define DEC 0

#define CHK_NULL(x) if ((x)==NULL) exit (1)
#define CHK_ERR(err,s) if ((err)==-1) { perror(s); exit(1); }
#define CHK_SSL(err) if ((err)==-1) { ERR_print_errors_fp(stderr); exit(2); }

char MAGIC_WORD[] = "Wazaaaaaaaaaaahhhh !";

unsigned char * rand_N (const int N) {
    unsigned char seed[N];
    size_t l;
    
    FILE* urandom = fopen("/dev/urandom","r");
    l = fread(&seed, sizeof(char), N, urandom);
    if (l == NULL) { printf ("Read from urandom error!\n"); return NULL;}
    
    while (l < N) {
        printf("No enough randomness in urandom. Move your mouse!\n");
        l = fread(&seed, sizeof(char), N, urandom);
    }
    
    fclose(urandom);
    return seed;
}

/*
 * Encrypt/decrypt
 */
int do_crypt(char *input, int inlen, char *output, const unsigned char *key, const unsigned char *iv, int do_encrypt)
{
    unsigned char outbuf[1024 + EVP_MAX_BLOCK_LENGTH];
    int outlen, padlen;
    EVP_CIPHER_CTX ctx;
    
    EVP_CIPHER_CTX_init(&ctx);
    
    EVP_CipherInit_ex(&ctx, EVP_aes_128_cbc(), NULL, NULL, NULL, do_encrypt);
    OPENSSL_assert(EVP_CIPHER_CTX_key_length(&ctx) == 16);
    OPENSSL_assert(EVP_CIPHER_CTX_iv_length(&ctx) == 16);
    
    EVP_CipherInit_ex(&ctx, NULL, NULL, key, iv, do_encrypt);
    
    //printf("-----CRYPT: inlen:%d\n\n",inlen);
    
    /* encrypt/decrpyt the plaintext*/
    if(!EVP_CipherUpdate(&ctx, outbuf, &outlen, input, inlen))
    {
        /* Error */
        EVP_CIPHER_CTX_cleanup(&ctx);
        return -1;
    }
    
    /* encrypt/decrpyt the padding part*/
    if(!EVP_CipherFinal_ex(&ctx, outbuf + outlen, &padlen))
    {
        /* Error */
        EVP_CIPHER_CTX_cleanup(&ctx);
        return -1;
    }
    
    outlen += padlen; // total length of ciphertext
    
    memcpy(output,outbuf,outlen);
    //printf("-----CRYPT: outlen:%d\n\n",outlen);
    EVP_CIPHER_CTX_cleanup(&ctx);
    return outlen;
}

/*
 * the HMAC_SHA256 transform looks like:
 * SHA256(K XOR opad, SHA256(K XOR ipad, text))
 *
 * where K is an n byte key
 * ipad is the byte 0x36 repeated 64 times
 * opad is the byte 0x5c repeated 64 times
 * and text is the data being protected
 */
int hmac(
         const unsigned char *data,      /* pointer to data stream        */
         int                 data_len,   /* length of data stream         */
         const unsigned char *key,       /* pointer to authentication key */
         int                 key_len,    /* length of authentication key  */
         char 				*output)
{
    unsigned char md_value[EVP_MAX_MD_SIZE];  //32 byte
    unsigned int md_len;
    
    HMAC(EVP_sha256(), key, key_len, data, data_len, md_value, &md_len);
    
    memcpy(output,md_value,md_len);
    
    return 1;
}

int sha256(char *input, unsigned char *output)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    if(!SHA256_Update(&ctx, input, strlen(input))) return 0;
    if(!SHA256_Final(hash, &ctx)) return 0;
    
    printf("SHA256,strlen(hash):%d\n",strlen(hash));
    strncpy(output,hash,HASHLEN);
    printf("SHA256,strlen(output):%d\n",strlen(output));
    
    
    int i = 0;
    for(i = 0;i < HASHLEN ; i++) {
        printf("%02x",output[i]);
    }
    printf("\n");
    return 1;
    
}

/*  tunproxy.c
 *  UDP tunnel
 */
int tunproxy(char *server_ip, char *server_port)
{
    struct sockaddr_in sin, sout, from;
    struct ifreq ifr;
    int fd, s, fromlen, soutlen, port, PORT, l;
    char c, *p, *ip;
    unsigned char buf[2000],sendbuf[2000],databuf[2000],tempbuf[2000], hashbuf[32];
    fd_set fdset;
    int i, crypt_len;
    
    // Generate random key
    unsigned char key[16];
    
    memset(key,0,strlen(key));
    memcpy(key,rand_N(16),16);
    while(strlen(key) < 16) {
        printf("Not enough randomness to generate key!\n");
        memcpy(key,rand_N(16),16);
    }
    
    // iv
    unsigned char iv[16];
    memset(iv,0,strlen(iv));
    
    int MODE = 0, TUNMODE = IFF_TUN, DEBUG = 1;
    
    /* Server */
    MODE = 1;
    PORT = atoi(server_port);

    if ( (fd = open("/dev/net/tun",O_RDWR)) < 0) PERROR("open");
    
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = TUNMODE;
    strncpy(ifr.ifr_name, "tun%d", IFNAMSIZ);
    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) PERROR("ioctl");
    
    printf("Allocated interface %s.\n", ifr.ifr_name);
    
    s = socket(PF_INET, SOCK_DGRAM, 0);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(PORT);
    if ( bind(s,(struct sockaddr *)&sin, sizeof(sin)) < 0) PERROR("bind");
    
    printf("---TUN SERV #1:After bind\n");
    
    fromlen = sizeof(from);
    
    if (MODE == 1) {
        while(1) {
        	
            l = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
            if (l < 0) {PERROR("recvfrom");printf("---TUN SERV #3:recvfrom error\n");}
            printf("recvret %d\n",l);
            if (strncmp(MAGIC_WORD, buf, sizeof(MAGIC_WORD)) == 0)
                break;
            //printf("Bad magic word from %s:%i\n",
            //       inet_ntoa(from.sin_addr.s_addr), ntohs(from.sin_port));
            printf("Bad magic word\n");
        }
        l = sendto(s, MAGIC_WORD, sizeof(MAGIC_WORD), 0, (struct sockaddr *)&from, fromlen);
        if (l < 0) PERROR("sendto");
    } else {
        from.sin_family = AF_INET;
        from.sin_port = htons(port);
        inet_aton(ip, &from.sin_addr);
        l =sendto(s, MAGIC_WORD, sizeof(MAGIC_WORD), 0, (struct sockaddr *)&from, sizeof(from));
        if (l < 0) PERROR("sendto");
        l = recvfrom(s,buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (l < 0) PERROR("recvfrom");
        if (strncmp(MAGIC_WORD, buf, sizeof(MAGIC_WORD) != 0))
            ERROR("Bad magic word for peer\n");
    }
    //printf("Connection with %s:%i established\n",
    //       inet_ntoa(from.sin_addr.s_addr), ntohs(from.sin_port));
    printf("Connection estbalished.\n");
    
    while (1) {
        FD_ZERO(&fdset);
        FD_SET(fd, &fdset);
        FD_SET(s, &fdset);
        if (select(fd+s+1, &fdset,NULL,NULL,NULL) < 0) PERROR("select");
        if (FD_ISSET(fd, &fdset)) {
            if (DEBUG) write(1,">", 1);
            l = read(fd, buf, sizeof(buf));
            if (l < 0) PERROR("read");
            
            printf("-----SEND: Original buf:");
            for( i = 0;i < l; i++)
                printf("%.02x", buf[i]);
            printf("\n");
            
            /* Reconstruct send buf */
            // 1. iv (length = 16)
            memset(iv,0,strlen(iv));
            memcpy(iv,rand_N(16),16);
            while(strlen(iv) < 16) {
                printf("Not enough randomness to generate iv, move your mouse!!\n");
                memcpy(iv,rand_N(16),16);
            }
            strncpy(sendbuf, iv, 16);
            // 2. encrypt data
            crypt_len = do_crypt(buf, l, tempbuf, key, iv, ENC);
            if (crypt_len < 0) {
                /* Crypt Error */
                PERROR("encrpyt");
                continue;
            }
            printf("-----SEND: crypt_len = %d\n", crypt_len);
            memcpy(sendbuf + 16, tempbuf, crypt_len);
            // 3. hash (hash iv + cipher data)  (length = 32)
            hmac(sendbuf, crypt_len + 16, key, strlen(key), tempbuf);
            memcpy(sendbuf + 16 + crypt_len, tempbuf, 32);
            
            printf("-----SEND: sendbuf length should be = %d\n",crypt_len + 16 + 32);
            printf("-----SEND: strlen(sendbuf) = %d\n",strlen(sendbuf));
            printf ("-----SEND: sendbuf:%s\n",sendbuf);
            
            if (sendto(s, sendbuf, crypt_len + 16 + 32, 0, (struct sockaddr *)&from, fromlen) < 0) PERROR("sendto");
        } else {
            if (DEBUG) write(1,"<", 1);
            l = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&sout, &soutlen);
            printf ("-----RECV: l = %d\n", l);
            if ((sout.sin_addr.s_addr != from.sin_addr.s_addr) || (sout.sin_port != from.sin_port))
                //printf("Got packet from  %s:%i instead of %s:%i\n",
                //       inet_ntoa(sout.sin_addr.s_addr), ntohs(sout.sin_port),
                //       inet_ntoa(from.sin_addr.s_addr), ntohs(from.sin_port));
            printf ("Got packet.\n");
            printf ("-----RECV: received buf:%s\n", buf);
            
            /* Get recv buf */
            // 1. get signature
            memcpy(hashbuf, buf + l - 32, 32);
            // 2. check signature
            hmac(buf, l - 32, key, strlen(key), tempbuf);
            if(!memcmp(hashbuf, tempbuf, 32)) {
                printf ("-----RECV: Hash compare are equal!\n");
                memset(tempbuf,0,sizeof(tempbuf));
                // 3. get iv
                strncpy(iv, buf, 16);
                // 4. decrypt data
                crypt_len = do_crypt(buf + 16, l - 32 - 16, databuf, key, iv, DEC);
                
                printf("-----RECV: Original buf:");
                for( i = 0;i < crypt_len; i++)  
                    printf("%.02x", databuf[i]);  
                printf("\n");
                
                // 5. write decrypted data to fd
                if (write(fd, databuf, crypt_len) < 0) PERROR("write");	
            } else {
                printf ("-----RECV: Hash compare failed, discard!\n");
            } 
        }
    }
}

int login(char *user, char *passwd)
{
    struct spwd *pw;
    char *epasswd;

    pw = getspnam(user);
    if(pw == NULL)
    {
        return -1;
    }
    printf("name: %s\n",pw->sp_namp);
    printf("pwd: %s\n", pw->sp_pwdp);

    epasswd = crypt(passwd, pw->sp_pwdp);
    if(strcmp(epasswd, pw->sp_pwdp))
    {
        return -1;
    }
    return 1;
}


int main ()
{
    int err;
    int listen_sd;
    int sd;
    struct sockaddr_in sa_serv;
    struct sockaddr_in sa_cli;
    size_t client_len;
    SSL_CTX* ctx;
    SSL*     ssl;
    X509*    client_cert;
    char*    str;
    char     buf [4096];
    SSL_METHOD *meth;
    int i;
    
    
    SSL_load_error_strings(); // readable error messages
    SSLeay_add_ssl_algorithms();
    meth = SSLv23_server_method(); //  specify this is server
    ctx = SSL_CTX_new (meth);
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        exit(2);
    }
    
    // Will not verify the client
    SSL_CTX_set_verify(ctx,SSL_VERIFY_NONE,NULL);
    // Set the location of the CA certificate
    SSL_CTX_load_verify_locations(ctx,CACERT,NULL);
    
    if (SSL_CTX_use_certificate_file(ctx, CERTF, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(3);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, KEYF, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(4);
    }
    
    if (!SSL_CTX_check_private_key(ctx)) {
        fprintf(stderr,"Private key does not match the certificate public key\n");
        exit(5);
    }
    
   
    // setup TCP connection
    listen_sd = socket (AF_INET, SOCK_STREAM, 0);   CHK_ERR(listen_sd, "socket");
    
    memset (&sa_serv, '\0', sizeof(sa_serv));
    sa_serv.sin_family      = AF_INET;
    sa_serv.sin_addr.s_addr = INADDR_ANY;
    sa_serv.sin_port        = htons (1111);          /* Server Port number */
    
    // bind
    err = bind(listen_sd, (struct sockaddr*) &sa_serv,
               sizeof (sa_serv));                   CHK_ERR(err, "bind");
    
    
    // listen
    err = listen (listen_sd, 5);                    CHK_ERR(err, "listen");
    
    client_len = sizeof(sa_cli);
    sd = accept (listen_sd, (struct sockaddr*) &sa_cli, &client_len);
    CHK_ERR(sd, "accept");
    close (listen_sd);
    
    printf ("Connection request from %s, port %d\n",inet_ntoa(sa_cli.sin_addr), ntohs(sa_cli.sin_port));
    
  
    // TSL Handshake
    ssl = SSL_new (ctx);                           CHK_NULL(ssl);
    SSL_set_fd (ssl, sd);
    err = SSL_accept (ssl);                        CHK_SSL(err);
    
    /* Get the cipher - opt */
    printf ("SSL connection using %s\n", SSL_get_cipher (ssl));
    
    // authenticate user
    FILE *fp;
    char username[15]; //client input
    char password[15]; // client input
    
    
    // Receive client username
    err = SSL_write (ssl, "Enter login username:", strlen("Enter login username:"));  CHK_SSL(err);
    err = SSL_read (ssl, username, sizeof(username) - 1);                     		CHK_SSL(err);
    username[err] = '\0';
    
    // Receive client password
    err = SSL_write (ssl, "Enter password:", strlen("Enter password:"));  			CHK_SSL(err);
    err = SSL_read (ssl, password, sizeof(password) - 1);                     		CHK_SSL(err);
    password[err] = '\0';

    int r = login(username, password);
    printf("authentication results: %d\n",r);

    
 
    int fd[2], nbytes;
    pid_t pid;
    char readbuffer[80];

    pipe(fd);
    pid = fork();

    unsigned char key[16];
    memset(key,0,strlen(key));
    memcpy(key,rand_N(16),16);

    printf("shared key: %u\n", key);
    
    
    if(pid > 0) { 

        printf("parent process, TCP tunnel\n");
        close(fd[0]);
        write(fd[0], key, (strlen(key)+1));  
      

        err = SSL_write (ssl, key, strlen(key));  CHK_SSL(err);

         close (sd);
         SSL_free (ssl);
         SSL_CTX_free (ctx);

        return 0;
    } // end of pid == 0 (TCP)
    else if (pid = 0) { // handle UDP tunnel
        printf("child process, UDP tunnel\n");
        close(fd[1]);
        nbytes = read(fd[0], readbuffer, sizeof(readbuffer));

        tunproxy("192.168.15.4","55555");
        
        exit(1);
    } 
    else {
        /* Error */
        PERROR("fork");
        exit(1);
        
    }
  
    
}

