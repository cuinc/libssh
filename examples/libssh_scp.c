/* libssh_scp.c
 * Sample implementation of a SCP client
 */

/*
Copyright 2009 Aris Adamantiadis

This file is part of the SSH Library

You are free to copy this file, modify it in any way, consider it being public
domain. This does not apply to the rest of the library though, but it is
allowed to cut-and-paste working code from this file to any license of
program.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include <libssh/libssh.h>
#include "examples_common.h"

char **sources;
int nsources;
char *destination;
int verbosity=0;

struct location {
  int is_ssh;
  char *user;
  char *host;
  char *path;
  ssh_session session;
  ssh_scp scp;
  FILE *file;
};

enum {
  READ,
  WRITE
};

static void usage(const char *argv0){
  fprintf(stderr,"Usage : %s [options] [[user@]host1:]file1 ... \n"
      "                               [[user@]host2:]destination\n"
      "sample scp client - libssh-%s\n",
//      "Options :\n",
//      "  -r : use RSA to verify host public key\n",
      argv0,
      ssh_version(0));
  exit(0);
}

static int opts(int argc, char **argv){
  int i;
  while((i=getopt(argc,argv,"v"))!=-1){
    switch(i){
      case 'v':
        verbosity++;
        break;
      default:
        fprintf(stderr,"unknown option %c\n",optopt);
        usage(argv[0]);
        return -1;
    }
  }
  nsources=argc-optind-1;
  if(nsources < 1){
    usage(argv[0]);
    return -1;
  }
  sources=malloc((nsources + 1) * sizeof(char *));
  if(sources == NULL)
    return -1;
  for(i=0;i<nsources;++i){
    sources[i] = argv[optind];
    optind++;
  }
  sources[i]=NULL;
  destination=argv[optind];
  return 0;
}

static ssh_session connect_ssh(char *host, char *user){
  ssh_session session;
  ssh_options options;
  int auth=0;

  options=ssh_options_new();
  if(user != NULL){
    if (ssh_options_set_username(options,user) < 0) {
      ssh_options_free(options);
      return NULL;
    }
  }

  if (ssh_options_set_host(options,host) < 0) {
    ssh_options_free(options);
    return NULL;
  }
  ssh_options_set_log_verbosity(options,verbosity);
  session=ssh_new();
  ssh_set_options(session,options);
  if(ssh_connect(session)){
    fprintf(stderr,"Connection failed : %s\n",ssh_get_error(session));
    ssh_disconnect(session);
    return NULL;
  }
  if(verify_knownhost(session)<0){
    ssh_disconnect(session);
    return NULL;
  }
  auth=authenticate_console(session);
  if(auth==SSH_AUTH_SUCCESS){
    return session;
  } else if(auth==SSH_AUTH_DENIED){
    fprintf(stderr,"Authentication failed\n");
  } else {
    fprintf(stderr,"Error while authenticating : %s\n",ssh_get_error(session));
  }
  ssh_disconnect(session);
  return NULL;
}

static struct location *parse_location(char *loc){
  struct location *location=malloc(sizeof(struct location));
  char *ptr;

  location->host=location->user=NULL;
  ptr=strchr(loc,':');
  if(ptr != NULL){
    location->is_ssh=1;
    location->path=strdup(ptr+1);
    *ptr='\0';
    ptr=strchr(loc,'@');
    if(ptr != NULL){
      location->host=strdup(ptr+1);
      *ptr='\0';
      location->user=strdup(loc);
    } else {
      location->host=strdup(loc);
    }
  } else {
    location->is_ssh=0;
    location->path=strdup(loc);
  }
  return location;
}

static int open_location(struct location *loc, int flag){
  if(loc->is_ssh && flag==WRITE){
    loc->session=connect_ssh(loc->host,loc->user);
    if(!loc->session){
      fprintf(stderr,"Couldn't connect to %s\n",loc->host);
      return -1;
    }
    loc->scp=ssh_scp_new(loc->session,SSH_SCP_WRITE,loc->path);
    if(!loc->scp){
      fprintf(stderr,"error : %s\n",ssh_get_error(loc->session));
      return -1;
    }
    if(ssh_scp_init(loc->scp)==SSH_ERROR){
      fprintf(stderr,"error : %s\n",ssh_get_error(loc->session));
      ssh_scp_free(loc->scp);
      return -1;
    }
    return 0;
  } else if(loc->is_ssh && flag==READ){
    loc->session=connect_ssh(loc->host, loc->user);
    if(!loc->session){
      fprintf(stderr,"Couldn't connect to %s\n",loc->host);
      return -1;
    }
    loc->scp=ssh_scp_new(loc->session,SSH_SCP_READ,loc->path);
    if(!loc->scp){
      fprintf(stderr,"error : %s\n",ssh_get_error(loc->session));
      return -1;
    }
    if(ssh_scp_init(loc->scp)==SSH_ERROR){
      fprintf(stderr,"error : %s\n",ssh_get_error(loc->session));
      ssh_scp_free(loc->scp);
      return -1;
    }
    return 0;
  } else {
    loc->file=fopen(loc->path,flag==READ ? "r":"w");
    if(!loc->file){
    	if(errno==EISDIR){
    		if(chdir(loc->path)){
    			fprintf(stderr,"Error changing directory to %s: %s\n",loc->path,strerror(errno));
    			return -1;
    		}
    		return 0;
    	}
    	fprintf(stderr,"Error opening %s: %s\n",loc->path,strerror(errno));
    	return -1;
	}
    return 0;
  }
  return -1;
}

/** @brief copies files from source location to destination
 * @param src source location
 * @param dest destination location
 * @param recursive Copy also directories
 */
static int do_copy(struct location *src, struct location *dest, int recursive){
  int size;
  socket_t fd;
  struct stat s;
  int w,r;
  char buffer[16384];
  int total=0;
  int mode;
  char *filename;
  /* Get the file name and size*/
  if(!src->is_ssh){
    fd=fileno(src->file);
    fstat(fd,&s);
    size=s.st_size;
    mode=s.st_mode;
    filename=ssh_basename(src->path);
  } else {
    size=0;
    do {
    	r=ssh_scp_pull_request(src->scp);
    	if(r==SSH_SCP_REQUEST_NEWDIR){
    		ssh_scp_deny_request(src->scp,"Not in recursive mode");
    		continue;
    	}
    	if(r==SSH_SCP_REQUEST_NEWFILE){
    		size=ssh_scp_request_get_size(src->scp);
    		filename=strdup(ssh_scp_request_get_filename(src->scp));
    		mode=ssh_scp_request_get_permissions(src->scp);
    		//ssh_scp_accept_request(src->scp);
    		break;
    	}
    	if(r==SSH_ERROR){
    		fprintf(stderr,"Error: %s\n",ssh_get_error(src->session));
    		return -1;
    	}
    } while(r != SSH_SCP_REQUEST_NEWFILE);
  }

  if(dest->is_ssh){
	  r=ssh_scp_push_file(dest->scp,src->path,size,0644);
	  //  snprintf(buffer,sizeof(buffer),"C0644 %d %s\n",size,src->path);
	  if(r==SSH_ERROR){
		  fprintf(stderr,"error: %s\n",ssh_get_error(dest->session));
		  ssh_scp_free(dest->scp);
		  return -1;
	  }
  } else {
	  if(!dest->file){
		  dest->file=fopen(filename,"w");
		  if(!dest->file){
			  fprintf(stderr,"Cannot open %s for writing: %s\n",filename,strerror(errno));
			  if(src->is_ssh)
				  ssh_scp_deny_request(src->scp,"Cannot open local file");
			  return -1;
		  }
	  }
	  if(src->is_ssh){
		  ssh_scp_accept_request(src->scp);
	  }
  }
  do {
	  if(src->is_ssh){
		  r=ssh_scp_read(src->scp,buffer,sizeof(buffer));
		  if(r==SSH_ERROR){
			  fprintf(stderr,"Error reading scp: %s\n",ssh_get_error(src->session));
			  return -1;
		  }
	  } else {
		  r=fread(buffer,1,sizeof(buffer),src->file);
		  if(r==0)
			  break;
		  if(r<0){
			  fprintf(stderr,"Error reading file: %s\n",strerror(errno));
			  return -1;
		  }
	  }
	  if(dest->is_ssh){
		  w=ssh_scp_write(dest->scp,buffer,r);
		  if(w == SSH_ERROR){
			  fprintf(stderr,"Error writing in scp: %s\n",ssh_get_error(dest->session));
			  ssh_scp_free(dest->scp);
			  dest->scp=NULL;
			  return -1;
		  }
	  } else {
		  w=fwrite(buffer,r,1,dest->file);
		  if(w<=0){
			  fprintf(stderr,"Error writing in local file: %s\n",strerror(errno));
			  return -1;
		  }
	  }
	  total+=r;

  } while(total < size);
  printf("wrote %d bytes\n",total);
  if(dest->is_ssh){
	  r=ssh_scp_close(dest->scp);
	  if(r == SSH_ERROR){
		  fprintf(stderr,"Error closing scp: %s\n",ssh_get_error(dest->session));
		  ssh_scp_free(dest->scp);
		  dest->scp=NULL;
		  return -1;
	  }
  } else {
	  fclose(dest->file);
	  dest->file=NULL;
  }
  return 0;
}

int main(int argc, char **argv){
  struct location *dest, *src;
  int i;

  if(opts(argc,argv)<0)
    return EXIT_FAILURE;
  dest=parse_location(destination);
  if(open_location(dest,WRITE)<0)
    return EXIT_FAILURE;
  for(i=0;i<nsources;++i){
    src=parse_location(sources[i]);
    if(open_location(src,READ)<0){
      return EXIT_FAILURE;
    }
    if(do_copy(src,dest,0) < 0)
      return EXIT_FAILURE;
  }
  ssh_disconnect(dest->session);
  ssh_finalize();
  return 0;
}
