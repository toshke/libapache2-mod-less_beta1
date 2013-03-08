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

#include "md5.h"

#include <stdio.h>
#include <sys/stat.h>
#include <sys/shm.h>

#define SHM_KEY 734253987
#define MAP_NUMBER_OF_FILES
#define info_log(ERROR,args...) if(server != NULL) ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, ERROR, args);
#define data_list_STEP 5
#define MAX_DEPENDENCIES 15

typedef enum { NUMBER = 0,STRING = 1 } data_type;

typedef union string_or_int {
	int number;
	char* string;
} string_or_int;

typedef struct list_member {
	data_type type;
	string_or_int data;
	struct list_member* next;
	struct list_member* prev;
} list_member;


typedef struct data_list {
	int size;
	struct list_member* first;
	struct list_member* last;
} data_list;


char static *source = NULL;
const static server_rec  *server = NULL;

/*
	Initialized data list. String lists are as simple as it gets, double linked lists with head & tail
*/
data_list* data_list_init(){
	data_list* list;
  
	list = malloc(sizeof(data_list));
	list->first = NULL;
	list->last = NULL;
	
	return list;
}


/*
	Adds string element to the end of the data list. 
*/
list_member* data_list_add_string(data_list* list, char* string){
	list_member* memb;
	char* string_copy;
	
	if(list == NULL){
		return;
	}
	string_copy = malloc(sizeof(char) * (strlen(string)+1));
	strcpy(string_copy,string);
	
	memb = list->last;

	if(memb == NULL){
		memb = malloc(sizeof(list_member));
		memb->type = STRING;
		memb->data.string = string_copy;
		
		memb->prev = NULL;
		memb->next = NULL;
		
		list->first = memb;
		list->last = memb;
	} else {
		while(memb->next != NULL) {
			memb = memb->next;
		}
		memb->next = malloc(sizeof(list_member));
		memb->type = STRING;
		memb->next->data.string = string_copy;
		
		memb->next->prev = memb;
		memb->next->next = NULL;
		
		list->last = memb->next;
		memb = memb->next;
	}
	list->size = list->size + 1;
	
	return memb;
}


list_member* data_list_add_number_sorted(data_list* list, int number){
	list_member* left;
	list_member* right;
	list_member* new;
	
	new = malloc(sizeof(data_list));
	if(new == NULL){
		info_log("Add number sorted to list failed",NULL); 
	}
	
	new->type = NUMBER;
	new->data.number = number;
	new->next = new->prev = NULL;
	
	if(list == NULL){ 
		return ;
	}
	
	if(list->size == 0){
		list->first = list->last = new;
		list->size = 1;
		return;
	}
	
	//make it a head
	if(list->first->data.number > number){
		new->next = list->first;
		list->first->prev = new;
		list->first = new;
		list->size++;
		return;
	} 
	
	//make it a tail
	if(list->last->data.number < number){		
		new->prev = list->last;
		list->last->next = new;
		list->last = new;
		list->size++;
		return;
	}
	
	
	//determine where to put new element is more than two elements in list
	left = list->first;
	right = left->next;
	//right should neve be NULL 'cause of previous statements, we're walking within inner bounds here
	//still, for security reasons, leavin condition
	while(right != NULL && left->data.number < number){
		right = right->next;
		left = right->prev;
	}	
	left->next = new;
	right->prev = new;
	
	new->prev = left;
	new->next = right;
	
	list->size++;
	return new;	
}


void data_list_print_members(data_list* list){
	list_member* member;
	//this is test method anyway, so just allocating 1KB for string to be printed
	char message[1024];
	char temp_number[20];
	char* current;
	if(list == NULL || list->first == NULL) {		
		return;
	}
	
	strcpy(message,"[");
	
    member	= list->first;
	do{
		info_log("here %s",member->type == STRING ? "num" : "str");
		if(member->type == NUMBER){
			sprintf(temp_number,"%d",member->data.number);
			current = temp_number;
		} else{
			current = member->data.string;
		}		
		strcat(message,current);
		strcat(message," | ");
	}while((member = member->next) != NULL);
	info_log(message,NULL);
}


void data_list_print_all(data_list* list,char* format_string){
	list_member* member;
	
	if(list == NULL || list->first == NULL) {		
		return;
	}
	
    member	= list->first;
	do{
		info_log(format_string,member->type == NUMBER ? member->data.number : member->data.string  );
	}while((member = member->next) != NULL);
}

/*
	String function, returns directory name for given file name. 
	Second paramter determines whatever trailing slash should be added or not
*/
char* directory_name(char* filename,int trailing_slash){
	int fname_len,buffer_length;
	char* directory_name;
	trailing_slash = trailing_slash ? 1 : 0;
	fname_len = strlen(filename)-1;
	while(filename[fname_len--] != '/');
	
	buffer_length = fname_len+trailing_slash+2;	
	directory_name = malloc(sizeof(char)* (buffer_length));
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
int scan_less_file_for_dependencies(char* less_file_name,data_list* dependencies_list ){
	int left=0,
	right=6,
	length=16,
	buffer_size,
	i=0;
	char* less_source;
	char* dirname;
	
	
	
	dirname = directory_name(less_file_name,0);
	less_source = read_file(less_file_name);
	
	if(less_source == NULL){	
		return;
	}
	
	buffer_size = strlen(less_source);
			info_log("Scanning %s for deps",less_file_name);
	while(right < buffer_size){		
		detect_dependencies(left,right,less_source, dependencies_list, dirname);		
		right++;
		if(right - left > length){
			left++;
		}
	}

	free(less_source);	
	free(dirname);	
}

int detect_dependencies(int left_ptr,int right_ptr,char* stream,data_list* dependencies_list,char* root){
	char* patterns[] = { "@import", "@import-once", "@import-multiple" };
	int i=0,j=0,k=0,source_length=0, pattern_length=0;
	char dependency[160];
	source_length = strlen(stream);
	
	for(i=0;i<3;i++){
		j=0;
		pattern_length = strlen(patterns[i]);
		while(left_ptr < source_length &&
			  left_ptr >= 0 &&
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
			
			
			//add dependencies recursively
			char* full_dependency_name;
				
			asprintf(&full_dependency_name,"%s/%s",root,dependency);
			
			if(!file_exists(full_dependency_name)){
				free(full_dependency_name);
				asprintf(&full_dependency_name,"%s/%s%s",root,dependency,".less");
			}
			
			
			data_list_add_string(dependencies_list,full_dependency_name);	
		
			scan_less_file_for_dependencies(full_dependency_name,dependencies_list);	
					
			free(full_dependency_name);
				
		}
	}
}

char* compiled_file_name(char* less_file_name){
	
	char* file_name;	
	char last_part[80];
	int i = strlen(less_file_name);
	int j = 0;
	while( (last_part[j++] = less_file_name[--i]) != '/');
	last_part[j-1]='\0';
	
	asprintf(&file_name,"/tmp/%s_compiled.css",last_part);
	info_log(file_name,NULL);
	return file_name;
}

int needs_to_be_compiled(char *less_file_name,data_list* list){
	char* compile_file_name;
	int fresh = 1;
	list_member* element;
	
	compile_file_name = compiled_file_name(less_file_name);
	element = list ? list->first : NULL;
	
	struct stat source_info;
	struct stat compiled_info;
	struct stat depended_info;
	
	fresh = fresh && (stat(less_file_name,&source_info)==0) && (stat(compile_file_name,&compiled_info) ==0);
	fresh = fresh && compiled_info.st_mtime > source_info.st_mtime;
	
	
	while(fresh && element != NULL){
		//compiled file must be younger than file it depends on 
		fresh = fresh && (stat((element->data).string,&depended_info)==0) && compiled_info.st_mtime > depended_info.st_mtime;
		element = element->next;
		if(!fresh) info_log("Dependecy newer: %s",(element->data).string);
	}
	return !fresh;
}

static int less_handler(request_rec* r){

	char * compile_file_name = NULL;
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
	
	//just testing sorted list
/*	data_list* list;

	list = data_list_init();

	data_list_add_number_sorted(list,1);
	data_list_add_number_sorted(list,11);
	data_list_add_number_sorted(list,15);
	data_list_add_number_sorted(list,-350);
	data_list_add_number_sorted(list, 275);
	data_list_add_number_sorted(list,-15);
	data_list_add_number_sorted(list,-7);
	data_list_add_number_sorted(list,0);
	data_list_print_members(list);return; */
	
	
	
	
	compile_file_name = compiled_file_name(r->filename);
	asprintf(&command, "lessc %s > %s", r->filename, compile_file_name);
	

	if (!file_exists(compile_file_name)){
		status = system(command);
		if(status != 0){
			free(compile_file_name);
			free(command);
			return DECLINED;
		}
	}

	if (file_exists(compile_file_name)){
		char* less_dir_name;
		struct data_list* dependencies = data_list_init();
		
		scan_less_file_for_dependencies(r->filename,dependencies);		
			
		if (needs_to_be_compiled(r->filename,dependencies)){
			info_log("NEWER",NULL);
			status = system(command);
			if(status != 0){
				free(compile_file_name);
				free(command);
				return DECLINED;
			}
		}
			
		if ((source = read_file(compile_file_name)) != NULL){			
			ap_set_content_type(r, "text/css");
        	ap_rputs(source,r);
			free(source);
			free(compile_file_name);
			free(command);
			return OK;

		} else {

			asprintf(&errornum, "%d", error);
			ap_set_content_type(r, "text/css");
        	ap_rputs(errornum,r);
			free(source);
			free(compile_file_name);
			free(command);
			return OK;

		}

	}
    	
}
 
static void register_hooks(apr_pool_t* pool){
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





