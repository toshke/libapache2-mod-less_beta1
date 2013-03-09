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

 /*
  *  tosicnikola10@gmail.com : TODO: 
  *  1) free dependencies list memory, there's memory leak right now
  *  2) sometimes calloc() in number list causes failure, further investigate
  *  3) filename shouldn't be just reversed file name, does not make any sense
  *     filepath should affect cached file name
  *  4) info_log writes to error file. Define different types of loggins
  *  5) Improvement: Cache dependency maps using shm and/or mmap between requests. 
  *     No need to parse all files upon each request (This modules is not intended for 
  *     production use anyway, so this is lower priority
  *  6) resolve @import dependenices file paths through apache api rather than building file path directly
  *  7) Log all above to github issues section
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

typedef struct int_array {
	int* start;
	int size; /* Acutal size of allcoated memory */
	int used_size; /* Used size */
	int initial_size;
} int_array;

char static *source = NULL;
const static server_rec  *server = NULL;

/**
	Initialized array of integers
**/
int_array* int_array_init(){
	int_array* array;
	
	array = malloc(sizeof(int_array));
	array->size = 0;
	array->used_size = 0;
	array->initial_size = 8;
	return array;
}

/**
	Adds number to array. If array is filled, array size is doubled.
	Returns new array size upon success, -1 on failure
**/
int int_array_add(int_array* array, int number){
	int new_size;
	//info_log("IN. Used: %d. Size: %d",array->used_size,array->size);
	if(array->size == array->used_size){
		new_size = array->size == 0 ? array->initial_size : (array->size) *2;
		//info_log("Extending buffer size to %d", new_size);
		array->start = (int*) realloc(array->start,sizeof(int) * new_size );
		if(array->start == NULL){
			return -1;
		}
		array->size = new_size;
		//info_log("Extended buffer size to %d", array->size);
	}
	//info_log("Add %d to arr. Used: %d. Size: %d",number,array->used_size,array->size);
	//write to actual memory locaiton
	*(array->start + array->used_size) =  number;
	
	//increase number of used memory locations
	array->used_size++;
	//info_log("OUT. Used: %d. Size: %d",array->used_size,array->size);
	return array->used_size;
}

/**
	Performs linear search on array. Returns position if needle, found, -1 if not
**/
int int_array_find(int_array* haystack,int needle){
	int i=0;
	if(haystack == NULL){
		return -1;
	}
	for(i=0;i<haystack->used_size;i++){
		
		if(*(haystack->start + i) == needle){
			return i;
		}
	}
	return -1;
}

/**
	Clears memory taken by int array
**/
void int_array_destroy(int_array* array){
	free(array->start);
	free(array);
}

char* int_array_to_string(int_array* array){
	//buffer size for string representation is 2(brackets) + 2 * n (for comas and spaces) + 10 * n, considering 
	// fact that integer goes anywhere between +-2billions (10numbers max) + 1 for terminating null
	char *buffer;
	int i;
	
	char number_buffer[11];
	buffer = malloc(sizeof(char) * (3 + 12 * array->used_size));
	buffer[0] = '\0';
	strcat(buffer,"[");
	for(i=0;i < array->used_size; i++){
		sprintf(number_buffer,"%d",(array->start)[i]);
		strcat(buffer,number_buffer);
		if(i < array->used_size -1)
			strcat(buffer,", ");
	}
	strcat(buffer,"]");
	
	
	return buffer;
}

void test_int_array(){
	int_array* arr = int_array_init();
    char* arr_str;
	int i=0;
	for( i = 2;i<100;i+=2){
		int_array_add(arr,i);
	}
	for(i=2;i<100;i++){
		if(int_array_find(arr,i)>=0){ info_log("Found %d at %d",i,int_array_find(arr,i));}else {info_log("%d not found",i);}
	}
	arr_str = int_array_to_string(arr);
	info_log("%s",arr_str);
	free(arr_str);
}



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
int scan_less_file_for_dependencies(char* less_file_name,data_list* dependencies_list,int_array* scanned_inodes ){
	int left=0,
	right=6,
	length=16,
	buffer_size,
	i=0;
	char* less_source;
	char* dirname;
	struct stat file_info;
	
	if(stat(less_file_name,&file_info) != 0){
		//file does not exist at all
		//TODO log warning 
		return; 
	}
	
	if(int_array_find(scanned_inodes,(int)file_info.st_ino) >= 0){
		//info_log("Skipped scanned file: %s with inode:%d", less_file_name,(int)file_info.st_ino);
		return;
	} else {
		int_array_add(scanned_inodes, (int)file_info.st_ino);
		//info_log("Added file: %s with inode: %d", less_file_name,(int)file_info.st_ino);
	} 
	
	dirname = directory_name(less_file_name,0);
	less_source = read_file(less_file_name);
	
	if(less_source == NULL){	
		return;
	}
	
	buffer_size = strlen(less_source);
	while(right < buffer_size){
		//if dependency detect, make enf of current region beginning of next one
		if(detect_dependencies(left,right,less_source, dependencies_list, dirname,scanned_inodes)){
			left=right;
			right+=length;
		} else {
			right++;		
			if(right - left > length){
				left++;
			}
		}
	
	}

	free(less_source);	
	free(dirname);	
}

int detect_dependencies(int left_ptr,
						int right_ptr,
						char* stream,
						data_list* dependencies_list,
						char* root,
						int_array* already_scanned){
	char* patterns[] = { "@import ", "@import-once", "@import-multiple" };
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
			i=3;
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
			//info_log("Call recursive: %d  %d", left_ptr, right_ptr);
			scan_less_file_for_dependencies(full_dependency_name,dependencies_list,already_scanned);	
			return 1;
			free(full_dependency_name);
				
		}
	}
	return 0;
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
		struct int_array* scanned_files = int_array_init();
		
		scan_less_file_for_dependencies(r->filename,dependencies,scanned_files);		
			
		//int_array_destroy(scanned_files);	
			
		if (needs_to_be_compiled(r->filename,dependencies)){
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





