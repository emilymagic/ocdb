#ifndef POSTGRES_FDB_TEST_S3ACCESS_H
#define POSTGRES_FDB_TEST_S3ACCESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nodes/pg_list.h"

typedef struct S3Objs
{
	List *objPathList;
} S3Objs;

typedef struct S3ObjKey
{
	char *bucketName;
	char *objectName;
} S3ObjKey;

typedef struct S3Obj
{
	uint32 size;
	char *data;
} S3Obj;

typedef struct S3Conf
{
	const char *user;
	const char *pwd;
	const char *region;
	const char *endpointOverride;
} S3Conf;

extern const char *default_bucket_name;

extern void *S3InitAccess();
extern void S3DestroyAccess(void *s3Client);
extern void S3CreateBucket(void *s3Client, const char *bucketPath);
extern void S3DeleteBucket(void *s3Client, const char *bucketPath);
extern S3Objs S3DeleteObjects(void *s3Client, const char *prefix);
extern S3Obj S3GetObject(void *s3Client, S3ObjKey s3_obj_key);
extern uint32 S3GetObject2(void *s3Client, const char *bucketPath,
							   const char *objPath, char *data);
extern void S3PutObject(void *s3Client, S3ObjKey s3_obj_key, S3Obj s3_obj);
extern void S3DeleteObject(void *s3Client, char *objPath);
extern bool S3BucketExist(void *s3Client, const char *bucketName);

#ifdef __cplusplus
};
#endif
#endif //POSTGRES_FDB_TEST_S3ACCESS_H
