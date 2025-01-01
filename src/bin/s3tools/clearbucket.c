#include "postgres.h"

#include "s3functions.h"

int
main(int argc, char **argv)
{
	void *s3Client;

	s3Client = S3InitAccess();

	S3DeleteObjects(s3Client, NULL);
	//S3DeleteBucket(s3Client, default_bucket_name);

	S3DestroyAccess(s3Client);
}
