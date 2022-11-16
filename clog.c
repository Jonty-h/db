/*
 * clog.c
 *
 *  Created on: 2022-6-11
 *      Author: liang
 */

#include <fcntl.h>
#include <sys/types.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include "clog.h"

#if 1
#define LOGD(fmt, args...)   printf("[%s-%s-%d] " fmt, __FILE__, __func__, __LINE__, ##args)
#define LOGE(info) perror(info)
#else
#define LOGD(fmt, args...)
#define LOGE(info)
#endif

//滚动日志文件，达到限制就另记一个日志文件
static void RollLogFile(CLogger_t *logger)
{
    int i;
    off_t size;
    if (logger->currSize >= logger->maxSize) {
        LOGD("currSize:%u, maxSize=%u\r\n", logger->currSize, logger->maxSize);
        //再次大小确认
        size = lseek(logger->fd, 0, SEEK_END);
        logger->currSize = (unsigned int)size>=0?size:0;
        if (logger->currSize < logger->maxSize) {
            return;
        }

        //开始滚动日志
        char file1[PATH_MAX] = {0};
        char file2[PATH_MAX] = {0};
        for (i = logger->fileCnt-2; 0 < i; i--) {
            snprintf(file1, sizeof(file1), "%s.%d", logger->path, i);
            snprintf(file2, sizeof(file2), "%s.%d", logger->path, i+1);
            if (F_OK == access(file2, F_OK)) {
                unlink(file2);
                LOGD("unlink %s\r\n", file2);
            }
            rename(file1, file2);//rename重命名不会更改文件的修改时间
            LOGD("rename %s %s\r\n", file1, file2);
        }

        snprintf(file2, sizeof(file2), "%s.1", logger->path);
        if (F_OK == access(file2, F_OK)) {
            unlink(file2);
            LOGD("unlink %s\r\n", file2);
        }

        if (0 <= logger->fd) {
            close(logger->fd);
            logger->fd = -1;
        }
        rename(logger->path, file2);
        LOGD("rename %s %s\r\n", logger->path, file2);

        logger->currSize = 0;
        logger->fd = open(logger->path, O_CREAT|O_RDWR|O_APPEND, 0666);
    }
}

//初始化日志记录器
CLogAck_e CLogInitLogger(CLogger_t *logger, const char* path)
{
#define DEFAULT_LOG_FILE_COUNT		5
#define DEFAULT_LOG_ONE_FILE_SIZE	(2*1024*1024)	//2M
#define DEFAULT_LOG_BUF_MAX_LEN		(2048)

    int fd;
    off_t size;
    fd = open(path, O_CREAT|O_RDWR|O_APPEND, 0666);
    if (0 > fd) {
        LOGE("open fail error:");
        return CLOG_ACK_FAIL;
    }

    logger->bufSize = (0 >= logger->bufSize)?DEFAULT_LOG_BUF_MAX_LEN:logger->bufSize;
    logger->buf = malloc(logger->bufSize);
    if (NULL == logger->buf) {
        LOGD("malloc fail\r\n");
        close(fd);
        return CLOG_ACK_FAIL;
    }

    size = lseek(fd, 0, SEEK_END);
    logger->fd = fd;
    logger->fileCnt = (0 >= logger->fileCnt)?DEFAULT_LOG_FILE_COUNT:logger->fileCnt;
    logger->maxSize = (0 >= logger->maxSize)?DEFAULT_LOG_ONE_FILE_SIZE:logger->maxSize;
    logger->currSize = (unsigned int)size>=0?size:0;

    memset(logger->path, 0x0, sizeof(logger->path));
    strncpy(logger->path, path, sizeof(logger->path));

    RollLogFile(logger);

    return CLOG_ACK_OK;
}

//释放日志记录器
void CLogUninitLogger(CLogger_t *logger)
{
    if (0 < logger->fd) {
        close(logger->fd);
        logger->fd = -1;
    }

    if (NULL != logger->buf) {
        free(logger->buf);
    }

    memset(logger, 0x0, sizeof(CLogger_t));
}

//记录日志信息接口
int CLog(CLogger_t *logger, const char *fmt, ...)
{
    int wLen;
    int buf_len;
    int tmp_len;
    time_t timep = {0};
    struct tm tmTime = {0};

    time(&timep);
    gmtime_r(&timep, &tmTime);

    snprintf(logger->buf, logger->bufSize, "#%02d-%02d-%02d %02d:%02d:%02d->",
             (1900+tmTime.tm_year), tmTime.tm_mon+1, tmTime.tm_mday,
             tmTime.tm_hour, tmTime.tm_min, tmTime.tm_sec);
    buf_len = strlen(logger->buf);

    va_list arglist;
    va_start(arglist, fmt);
    wLen = vsnprintf((char*)logger->buf+buf_len, logger->bufSize-buf_len, fmt, arglist);
    va_end(arglist);
    if (0 < wLen) {
        buf_len += wLen;
    } else {
        wLen = buf_len;
    }

    tmp_len = logger->maxSize - logger->currSize;//剩余空间, 可能会分两次写入
    if (0 < tmp_len && buf_len > tmp_len) {
        wLen = tmp_len;
    }
    wLen = write(logger->fd, logger->buf, wLen);
    if (0 < wLen) {
        logger->currSize += wLen;
        tmp_len = wLen;//记录已写入字节
        wLen = buf_len - wLen;
        RollLogFile(logger);
        if (0 < wLen) {//还有要写入的
            wLen = write(logger->fd, (char*)logger->buf+tmp_len, wLen);
            if (0 < wLen) {
                logger->currSize += wLen;
                wLen += tmp_len;
            }
            RollLogFile(logger);
        }
    } else {
        RollLogFile(logger);
    }

    return wLen;
}