#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include "clog.h"

// part 3 硬编码表的 字段长度
#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255

#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0)->Attribute)
//一个表最多只能有100个页 100 * 4k = 400k
#define TABLE_MAX_PAGES 100

typedef enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum {
    PREPARE_SUCCESS,
    PREPARE_UNRECOGNIZED_STATEMENT,
    PREPARE_NEGATIVE_ID,
    PREPARE_STRING_TOO_LONG,
    PREPARE_SYNTAX_ERROR
} PrepareResult;

typedef enum {
    EXECUTE_SUCCESS,
    EXECUTE_TABLE_FULL
} ExecuteResult;

typedef enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT
} StatementType;

typedef struct {
    char *buffer;
    size_t buffer_length;
    ssize_t input_length;
} InputBuffer;

typedef struct {
    uint32_t id;
    char username[COLUMN_USERNAME_SIZE + 1];
    char email[COLUMN_EMAIL_SIZE + 1];
} Row;

typedef struct {
    StatementType type;
    Row row_to_insert;  // only used by insert statement
} Statement;

typedef struct {
    int file_descriptor;
    uint32_t file_length;
    void *pages[TABLE_MAX_PAGES];
} Pager;

typedef struct {
    Pager* pager;
    uint32_t num_rows;
} Table;

//column	size (bytes)	offset
//id	    4	            0
//username	32	            4
//email	    255	            36
//total	    291

//row 紧凑表示
const uint32_t ID_SIZE = size_of_attribute(Row, id);
const uint32_t USERNAME_SIZE = size_of_attribute(Row, username);
const uint32_t EMAIL_SIZE = size_of_attribute(Row, email);
//ID_OFFSET 是字节数 offset
const uint32_t ID_OFFSET = 0;
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;
const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;
//页大小 4k
const uint32_t PAGE_SIZE = 4096;

//每页最多 (4096 / 291 ) 个 14 row
const uint32_t ROWS_PER_PAGE = PAGE_SIZE / ROW_SIZE;
//表最多有 14 * 100 = 1400 行
const uint32_t TABLE_MAX_ROWS = ROWS_PER_PAGE * TABLE_MAX_PAGES;

PrepareResult prepare_insert(InputBuffer *buffer, Statement *statement,CLogger_t logger);

void* get_page(Pager* pager, uint32_t page_num) {
    //如果请求页大于最大页数，直接报错
    if (page_num > TABLE_MAX_PAGES){
        printf("Tried to fetch page number out of bounds. %d > %d\n",
               page_num,TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    if (pager->pages[page_num] == NULL) {
        //说明内存中目前没有加载或者文件中也没有这个页
        void* page = malloc(PAGE_SIZE);

        //计算目前文件页数量
        uint32_t num_pages = pager->file_length / PAGE_SIZE;
        if (pager->file_length % PAGE_SIZE) {
            //最后一页不足14行
            num_pages += 1;
        }

        //如果文件中有对应的页，讲页内容读入缓存
        if (page_num <= num_pages){
            //修改文件指针至页起始位置
            lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
            ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
            if (bytes_read == -1){
                printf("Error reading file: %d\n", errno);
                exit(EXIT_FAILURE);
            }
        }

        pager->pages[page_num] = page;
    }

    return pager->pages[page_num];
}

//找出在内存中为特定行读/写的位置
void *row_slot(Table *table, uint32_t row_num) {
    //计算 row 在第几页
    uint32_t page_num = row_num / ROWS_PER_PAGE;
    //得到页指针
    void* page = get_page(table->pager, page_num);
    //余数 : row 在这页 的偏移量
    uint32_t row_offset = row_num % ROWS_PER_PAGE;
    //页内字节偏移
    uint32_t byte_offset = row_offset * ROW_SIZE;
    //页指针 + 字节偏移数
    return page + byte_offset;
}

//code to convert to and from the compact representation
//序列化row
void serialize_row(Row *source, void *destination) {
    //从源指向的位置直接复制到目标内存块
    //void *memcpy(void *destin, void *source, unsigned n)
    //从源source中拷贝n个字节到目标destin中
    memcpy(destination + ID_OFFSET, &(source->id), ID_SIZE);
    strncpy(destination + USERNAME_OFFSET, source->username, USERNAME_SIZE);
    strncpy(destination + EMAIL_OFFSET, source->email, EMAIL_SIZE);
}

//反序列化row
void deserialize_row(void *source, Row *destination) {
    memcpy(&(destination->id), source + ID_OFFSET, ID_SIZE);
    memcpy(&(destination->username), source + USERNAME_OFFSET, USERNAME_SIZE);
    memcpy(&(destination->email), source + EMAIL_OFFSET, EMAIL_SIZE);
}

InputBuffer *new_input_buffer() {
    InputBuffer *input_buffer = malloc(sizeof(InputBuffer));
    input_buffer->buffer = NULL;
    input_buffer->buffer_length = 0;
    input_buffer->input_length = 0;

    return input_buffer;
}

void print_prompt() { printf("db > "); }

void print_row(Row *row) {
    printf("(%d, %s, %s)\n", row->id, row->username, row->email);
}

void read_input(InputBuffer *input_buffer) {
    ssize_t bytes_read =
            getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);

    if (bytes_read <= 0) {
        printf("Error reading input\n");
        exit(EXIT_FAILURE);
    }

    //Ignore trailing newline
    //用户输入hjd -> buffer中是 hjd\n
    input_buffer->input_length = bytes_read - 1;
    input_buffer->buffer[bytes_read - 1] = 0;
}

void close_input_buffer(InputBuffer *input_buffer) {
    free(input_buffer->buffer);
    free(input_buffer);
}

void pager_flush(Pager* pager, uint32_t page_num, uint32_t size){
    if (pager->pages[page_num] == NULL) {
        printf("Tried to flush null page\n");
        exit(EXIT_FAILURE);
    }

    off_t offset = lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);

    if (offset == -1) {
        printf("Error seeking: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    ssize_t bytes_written =
            write(pager->file_descriptor, pager->pages[page_num], size);

    if (bytes_written == -1) {
        printf("Error writing: %d\n", errno);
        exit(EXIT_FAILURE);
    }
}

//TODO 目前大部分页缓存后，又原封不动的刷回盘
void db_close(Table* table){
    Pager* pager = table->pager;
    uint32_t num_full_pages = table->num_rows / ROWS_PER_PAGE;

    for (uint32_t i = 0; i < num_full_pages; i++) {
        if (pager->pages[i] == NULL){
            continue;
        }
        //刷盘
        pager_flush(pager, i, PAGE_SIZE);
        free(pager->pages[i]);
        pager->pages[i] = NULL;
    }

    uint32_t num_additional_rows = table->num_rows % ROWS_PER_PAGE;

    //最后一页 不足14行
    if (num_additional_rows > 0){
        uint32_t page_num = num_full_pages;
        if (pager->pages[page_num] != NULL) {
            pager_flush(pager,page_num,num_additional_rows * ROW_SIZE);
            free(pager->pages[page_num]);
            pager->pages[page_num] = NULL;
        }
    }

    int result = close(pager->file_descriptor);
    if (result == -1) {
        printf("Error closing db file.\n");
        exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        void* page = pager->pages[i];
        if (page) {
            free(page);
            pager->pages[i] = NULL;
        }
    }
    free(pager);
    free(table);
}

Pager* pager_open(const char * filename){
    int fd = open(filename,
                  O_RDWR |     // R W
                  O_CREAT, // create file if it does not exist
                  S_IWUSR |    // user W permission
                  S_IRUSR  // user R permission
    );
    if (fd == -1){
        printf("Unable to open file\n");
        exit(EXIT_FAILURE);
    }

    off_t file_length = lseek(fd, 0, SEEK_END);

    Pager *pager = malloc(sizeof(Pager));
    pager->file_descriptor = fd;
    pager->file_length = file_length;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pager->pages[i] = NULL;
    }

    return pager;
}

//initialize the table
Table *db_open(const char * filename) {
    Pager* pager = pager_open(filename);
    uint32_t num_rows = pager->file_length / ROW_SIZE;

    Table *table = (Table *) malloc(sizeof(Table));

    table->pager = pager;
    table->num_rows = num_rows;
    return table;
}

//执行insert
ExecuteResult execute_insert(Statement *statement, Table *table) {
    if (table->num_rows >= TABLE_MAX_ROWS) {
        return EXECUTE_TABLE_FULL;
    }

    //得到row
    Row *row_to_insert = &(statement->row_to_insert);

    //序列化row
    serialize_row(row_to_insert, row_slot(table, table->num_rows));
    table->num_rows += 1;

    return EXECUTE_SUCCESS;
}

//执行select
ExecuteResult execute_select(Statement *statement, Table *table) {
    Row row;
    for (uint32_t i = 0; i < table->num_rows; i++) {
        deserialize_row(row_slot(table, i), &row);
        print_row(&row);
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement *statement, Table *table) {
    switch (statement->type) {
        case (STATEMENT_INSERT):
            return execute_insert(statement, table);
        case (STATEMENT_SELECT):
            return execute_select(statement, table);
    }
}

PrepareResult prepare_insert(InputBuffer* input_buffer,
                             Statement* statement,
                             CLogger_t logger) {
    statement->type = STATEMENT_INSERT;

    char* keyword = strtok(input_buffer->buffer," ");
    char* id_string = strtok(NULL, " ");
    char* username = strtok(NULL, " ");
    char* email = strtok(NULL, " ");

    CLog(&logger, "username : %s, email : %s\r\n",
         username, email);
    CLog(&logger, "username length: %d, email length: %d\r\n",
         strlen(username), strlen(email));

    if (id_string == NULL || username == NULL || email == NULL){
        return PREPARE_SYNTAX_ERROR;
    }

    int id = atoi(id_string);
    if (id < 0) {
        return PREPARE_NEGATIVE_ID;
    }
    if (strlen(username) > COLUMN_USERNAME_SIZE ){
        return PREPARE_STRING_TOO_LONG;
    }
    if (strlen(email) > COLUMN_USERNAME_SIZE ){
        return PREPARE_STRING_TOO_LONG;
    }

    statement->row_to_insert.id = id;
    strcpy(statement->row_to_insert.username,username);
    strcpy(statement->row_to_insert.email,email);

    return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer *input_buffer,
                                Statement *statement,
                                CLogger_t logger) {
    if (strncmp(input_buffer->buffer, "insert", 6) == 0) {
        return prepare_insert(input_buffer, statement,logger);
    }
    if (strcmp(input_buffer->buffer, "select") == 0) {
        statement->type = STATEMENT_SELECT;
        return PREPARE_SUCCESS;
    }

    return PREPARE_UNRECOGNIZED_STATEMENT;
}

MetaCommandResult do_meta_command(InputBuffer *input_buffer, Table *table, CLogger_t logger) {
    if (strcmp(input_buffer->buffer, ".exit") == 0) {
        close_input_buffer(input_buffer);
        db_close(table);
        //释放日志记录器
        CLogUninitLogger(&logger);
        exit(EXIT_SUCCESS);
    } else {
        return META_COMMAND_UNRECOGNIZED_COMMAND;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }

    int ret;

    //配置最多记录多少个日志文件，每个日志文件大小
    CLogger_t logger = {
            .fileCnt = 1,		//1个日志文件
            .maxSize = 1024 * 100	//每个日志文件大小
    };

    //初始化日志记录器
    ret = CLogInitLogger(&logger, "C:\\Users\\hjd\\CLionProjects\\db\\length.log");
    if (CLOG_ACK_OK != ret) {
        printf("CLogInitLogger fail, ret:%d\r\n", ret);
    }

    char * filename = argv[1];
    Table* table = db_open(filename);

    InputBuffer *input_buffer = new_input_buffer();
    while (true) {
        print_prompt();
        read_input(input_buffer);

        if (input_buffer->buffer[0] == '.') {
            switch (do_meta_command(input_buffer, table, logger)) {
                case (META_COMMAND_SUCCESS):
                    continue;
                case (META_COMMAND_UNRECOGNIZED_COMMAND):
                    printf("Unrecognized command '%s'\n", input_buffer->buffer);
                    continue;
            }
        }

        Statement statement;
        switch (prepare_statement(input_buffer, &statement ,logger)) {
            case (PREPARE_SUCCESS):
                break;
            case (PREPARE_NEGATIVE_ID):
                printf("ID must be positive.\n");
                continue;
            case (PREPARE_STRING_TOO_LONG):
                printf("String is too long.\n");
                continue;
            case (PREPARE_SYNTAX_ERROR):
                printf("Syntax error. Could not parse statement.\n");
                continue;
            case (PREPARE_UNRECOGNIZED_STATEMENT):
                printf("Unrecognized keyword at start of '%s'.\n", input_buffer->buffer);
                continue;
        }
        switch (execute_statement(&statement, table)) {
            case (EXECUTE_SUCCESS):
                printf("Executed. \n");
                break;
            case (EXECUTE_TABLE_FULL):
                printf("Error: Table full. \n");
                break;
        }
    }
}
