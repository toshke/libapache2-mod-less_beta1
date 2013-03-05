/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_main.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_script.h"
#include "http_connection.h"

#include "apr_strings.h"
#include "apr_hash.h"

#include <stdio.h>
#include <sys/stat.h>
#include <sys/shm.h>

#define SHM_KEY 734253987
#define MAP_NUMBER_OF_FILES
#define info_log(ERROR,args...) if(server != NULL) ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, ERROR, args);


char static *source = NULL;

const static server_rec  *server = NULL;

const int shmid;

struct parent_less_file {
	char* file_name;
	time_t	modified_time;
};


apr_hash_t* get_dependency_map(){
	
}

void map_to_shm(){

}

void shm_to_map(){

}


int init_dependency_map(){

}

int set_file_dependencies(char* source,char ** dependencies){
	
}


int file_exists(const char * filename)
{
    FILE * file = fopen(filename, "r");
    if (file != NULL)
    {
        fclose(file);
        return 1;
    }
    return 0;
}


char* read_file(const char * filename){
	long bufsize;
	int count;
	int error = 0;
	char* message;
	char* loaded_chars;
	FILE * fp = fopen(filename, "r");
	if (fp != NULL) {
		if (fseek(fp, -1L, SEEK_END) == 0) {
			bufsize = ftell(fp) + 1;
				
			info_log("File %s size: %ld", filename, bufsize);

			loaded_chars = malloc(sizeof(char) * (bufsize + 1));
				
			if(loaded_chars == NULL){
				info_log("Memory buffer for file %s not allocated", filename);	
				return NULL;
			} else {
				
				info_log("Memory allocated!",NULL);		   
			}

			if (fseek(fp, 0L, SEEK_SET) == 0) 
				error = -1; 

			size_t newLen = fread(loaded_chars, sizeof(char), bufsize, fp);

			info_log("Read %u bytes from file",newLen);


			if (newLen == 0) {
				error = -1;
			} else {
				loaded_chars[newLen] = '\0';
			}
		}

	} 
	fclose(fp);

	if(error == -1) return loaded_chars;
	return NULL;
}




/**
	Scans loaded file for @import, @import-once and @import-multiple versions
	strlen(@import) = 7
	strlen(@import-once) = 12
	strlen(@import-multiple) =  16
	
**/
int scan_less_file_for_dependencies(char* less_file_name){
	int left=0,
	right=6,
	length=16,
	buffer_size;
	char* less_source;
	
	less_source = read_file(less_file_name);
	
	info_log("Scanning file %s for dependencies",less_file_name);
	
	if(source == NULL){
		//TODO log info
		return;
	}
	
	buffer_size = strlen(less_source);
	
	while(right < buffer_size){
		compare_in_source(left,right,less_source);
		right++;
		if(right - left > length){
			left++;
		}
	}
}

int compare_in_source(int left_ptr,int right_ptr,char* stream){
	char* patterns[] = { "@import", "@import-once", "@import-multiple" };
	int i=0,j=0,k=0,source_length=0, pattern_length=0;
	source_length = strlen(stream);
	char dependency[160];
	for(i=0;i<3;i++){
		j=0;
		pattern_length = strlen(patterns[i]);
		while(left_ptr < source_length &&
			  left_ptr > 0 &&
			  left_ptr < right_ptr &&
			  j < pattern_length &&
			  patterns[i][j] == stream[left_ptr+(j++)]){
			
		}
		if(j == pattern_length){
			char state = NULL,c = NULL;

			while((c = stream[left_ptr+j]) != ';' && left_ptr + (j++) < source_length){
				
				if(state == NULL && (c == '\'' || c == '"')) { 				
					state = c;k=0; 				
					continue;
				}
				if(state != NULL && c == state){
					break; 		
				}
				if(state!=NULL && c !=' ' && c!= '\t' && c!='\n'){
					dependency[k++] = c; 
				}
			}
			dependency[k] = '\0';		
			info_log("Depends on %s", dependency);
		}
	}
}



static int less_handler(request_rec* r)
{
	struct stat info;
	struct stat lessinfo;
	char * lessfilename = NULL;
	char * command = NULL;
	int status;
	int newer = 0;
	int error = 0;
	char * errornum = NULL;

	server = r->server;
	
	if (!r->handler || strcmp(r->handler, "less"))
	return DECLINED;

	if (r->method_number != M_GET)
	return HTTP_METHOD_NOT_ALLOWED;

	asprintf(&lessfilename, "%s_compiled.css", r->filename);
	asprintf(&command, "lessc %s > %s", r->filename, lessfilename);

	if (!file_exists(lessfilename)){

		status = system(command);
		if(status != 0)
			return DECLINED;

	}

	if (file_exists(lessfilename)){

		if ((stat(r->filename,&info) == 0) && (stat(lessfilename,&lessinfo) == 0) && (info.st_mtime > lessinfo.st_mtime))
			status = system(command);
			
		if ((source = read_file(lessfilename)) != NULL){
			scan_less_file_for_dependencies(r->filename);
			ap_set_content_type(r, "text/css");
        	ap_rputs(source,r);
			free(source);
			free(lessfilename);
			free(command);
			return OK;

		} else {

			asprintf(&errornum, "%d", error);
			ap_set_content_type(r, "text/css");
        	ap_rputs(errornum,r);
			free(source);
			free(lessfilename);
			free(command);
			return OK;

		}

	}
    	
}
 
static void register_hooks(apr_pool_t* pool)
{
    ap_hook_handler(less_handler, NULL, NULL, APR_HOOK_MIDDLE);
	
	init_dependency_map();
}
 
module AP_MODULE_DECLARE_DATA less_module = {
    STANDARD20_MODULE_STUFF,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    register_hooks
};
