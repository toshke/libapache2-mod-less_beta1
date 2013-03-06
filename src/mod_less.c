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
#define STRING_LIST_STEP 5
#define MAX_DEPENDENCIES 15

typedef struct string_list_member {
	char* string;
	struct string_list_member* next;
	struct string_list_member* prev;
} string_list_member;


typedef struct string_list {
	int size;
	struct string_list_member* first;
	struct string_list_member* last;
} string_list;


char static *source = NULL;
const static server_rec  *server = NULL;

string_list* string_list_init(){
	string_list* list;
    string_list_member* memb = NULL;
  
	list = malloc(sizeof(string_list));
	list->first = NULL;
	list->last = NULL;
	
	return list;
}

string_list_member* string_list_add(string_list* list, char* string){
	string_list_member* memb;
	char* string_copy;
	
	if(list == NULL){
		return;
	}
	string_copy = malloc(sizeof(char) * (strlen(string)+1));
	strcpy(string_copy,string);
	
	memb = list->last;

	if(memb == NULL){
		memb = malloc(sizeof(string_list_member));
		memb->string = string_copy;
		
		memb->prev = NULL;
		memb->next = NULL;
		
		list->first = memb;
		list->last = memb;
	} else {
		while(memb->next != NULL) {
			memb = memb->next;
		}
		memb->next = malloc(sizeof(string_list_member));
		memb->next->string = string_copy;
		
		memb->next->prev = memb;
		memb->next->next = NULL;
		
		list->last = memb->next;
		memb = memb->next;
	}
	list->size = list->size + 1;
	
	return memb;
}

void string_list_prepend_to_all(string_list* list,char* prepend_string){
	string_list_member* member;
	char* new_string;
	int new_length=0;
	if(list == NULL || list->first == NULL) {
		return;
	}
	
    member	= list->first;

	do{		
		new_length = strlen(prepend_string) + strlen(member->string) +1;
		new_string = malloc(sizeof(char) * new_length );
		strcpy(new_string,prepend_string);
		strcat(new_string,member->string);
		free(member->string);
		member->string = new_string;
		
	}while((member = member->next) != NULL);
}


void string_list_print_all(string_list* list,char* format_string){
	string_list_member* member;
	
	if(list == NULL || list->first == NULL) {		
		return;
	}
	
    member	= list->first;
	do{
		info_log(format_string,member->string);
	}while((member = member->next) != NULL);
}

char* directory_name(char* filename,int trailing_slash){
	int fname_len;
	char* directory_name;
	trailing_slash = trailing_slash ? 1 : 0;
	fname_len = strlen(filename)-1;
	while(filename[fname_len--] != '/');
		
	directory_name = malloc(sizeof(char)* (fname_len+1+trailing_slash));

	directory_name[fname_len + trailing_slash + 1] = '\0';
	do{
		directory_name[fname_len + trailing_slash] = filename[fname_len + trailing_slash];
	}while(--fname_len + trailing_slash >=0);
	
	return directory_name;
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
				
			loaded_chars = malloc(sizeof(char) * (bufsize + 1));
				
			if(loaded_chars == NULL){
				info_log("Memory buffer for file %s not allocated", filename);	
				return NULL;
			} else {			
				//info_log("Memory allocated!",NULL);		   
			}

			if (fseek(fp, 0L, SEEK_SET) == 0) 
				error = -1; 

			size_t newLen = fread(loaded_chars, sizeof(char), bufsize, fp);

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
int scan_less_file_for_dependencies(char* less_file_name,string_list* dependencies_list ){
	int left=0,
	right=6,
	length=16,
	buffer_size,
	i=0;
	char* less_source;
	
	less_source = read_file(less_file_name);
	
	if(less_source == NULL){
		
		return;
	}
	
	buffer_size = strlen(less_source);
	
	
	while(right < buffer_size){
		detect_dependencies(left,right,less_source, dependencies_list);
		right++;
		if(right - left > length){
			left++;
		}
	}
}

int detect_dependencies(int left_ptr,int right_ptr,char* stream,string_list* dependencies_list){
	char* patterns[] = { "@import", "@import-once", "@import-multiple" };
	int i=0,j=0,k=0,source_length=0, pattern_length=0;
	char dependency[160];
	source_length = strlen(stream);
	
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
			char state = '\0',c = '\0';

			while((c = stream[left_ptr+j]) != ';' && left_ptr + (j++) < source_length){
				
				if(state == '\0' && (c == '\'' || c == '"')) { 				
					state = c;k=0; 				
					continue;
				}
				if(state != '\0' && c == state){
					break; 		
				}
				if(state!='\0' && c !=' ' && c!= '\t' && c!='\n'){
					dependency[k++] = c; 
				}
			}
			dependency[k] = '\0';
			
			string_list_add(dependencies_list,dependency);
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
	int i =0;
	
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
		int stat_less_result, stat_compiled_result;
		char* less_dir_name;
		struct string_list* dependencies = string_list_init();
		
		scan_less_file_for_dependencies(r->filename,dependencies);		
		
		
		stat_less_result = stat(r->filename,&info);
		stat_compiled_result = stat(lessfilename,&lessinfo);
	
		less_dir_name = directory_name(lessfilename,1);
		
		string_list_prepend_to_all(dependencies, less_dir_name);
		string_list_print_all(dependencies,"DEPENDS: %s");
		free(less_dir_name);
	
		if ((stat_less_result == 0) && (stat_compiled_result == 0) && (info.st_mtime > lessinfo.st_mtime))
			status = system(command);
			
		if ((source = read_file(lessfilename)) != NULL){			
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
