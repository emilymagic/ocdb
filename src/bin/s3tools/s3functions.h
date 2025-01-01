#ifndef S3FUNCTIONS_H
#define S3FUNCTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nodes/pg_list.h"

typedef struct S3Objs
{
	List *objPathList;
} S3Objs;

typedef struct S3Obj
{
	uint32 size;
	char *data;
} S3Obj;

extern const char *default_bucket_name;

extern void *S3InitAccess();
extern void S3DestroyAccess(void *s3Client);
extern void S3DeleteBucket(void *s3Client, const char *bucketPath);
extern S3Objs S3DeleteObjects(void *s3Client, const char *prefix);
extern void S3DeleteObject(void *s3Client, const char *objPath);

#ifdef __cplusplus
};
#endif

#endif //S3FUNCTIONS_H
