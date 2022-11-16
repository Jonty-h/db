//
// Created by hjd on 2022/11/15.
//

#ifndef _CLOG_H_
#define _CLOG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>

typedef enum {
    CLOG_ACK_FAIL = -1,		//操作失败
    CLOG_ACK_OK = 0,		//操作成功
} CLogAck_e;

typedef struct {
    int fd;
    int fileCnt;			//滚动记录多少个日志文件, 如5个日志文件 a.log(当前正在记录的文件) a.log.1 a.log.2 a.log.3 a.log.4 按时间排序， 最新-->最旧
    unsigned int currSize;	//用户无需关心，当前日志文件大小
    unsigned int maxSize;	//一个日志文件的大小，满了之后另记一个文件
    char path[PATH_MAX];	//用户无需关心，日志文件路径
    int bufSize;			//每次写入日志数据最大的大小，
    void *buf;				//用户无需关心, 每次写入日志数据的缓冲区
} CLogger_t;

/******************************************
* 函数: CLogInitLogger
* 功能: 初始化日志记录器
* 参数: CLogger_t *logger：
*       const char* path：
* 输入:
* 输出:
* 返回: int 成功返回：CLOG_ACK_OK;其它返回：
* 说明: @xxx 2022.06.12 周日
******************************************/
CLogAck_e CLogInitLogger(CLogger_t *logger, const char* path);

/******************************************
* 函数: CLogUninitLogger
* 功能: 释放日志记录器
* 参数: CLogger_t *logger：
* 输入:
* 输出:
* 返回: void
* 说明: @xxx 2022.06.12 周日
******************************************/
void CLogUninitLogger(CLogger_t *logger);

/******************************************
* 函数: CLog
* 功能: 记录日志信息接口
* 参数: CLogger_t *logger：
*       const char *fmt：
*       ...：
* 输入:
* 输出:
* 返回: int 成功返回：写入字节数;其它返回：
* 说明: @xxx 2022.06.12 周日
******************************************/
int CLog(CLogger_t *logger, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* _CLOG_H_ */
