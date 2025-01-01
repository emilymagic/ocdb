#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/s3/model/DeleteBucketRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/DeleteObjectRequest.h>


extern "C" {
#include "postgres.h"
}

#include "s3functions.h"

using namespace Aws::S3;
using namespace Aws::Client;
using namespace Aws::Auth;

typedef struct S3Access
{
	S3Client *cli;
	Aws::SDKOptions *op;
} S3Access;

const char *default_bucket_name = "dbdata1";

static void
SetCofig(ClientConfiguration *conf)
{
	conf->region = "us-east-1";
	conf->scheme = Aws::Http::Scheme::HTTP;
	conf->verifySSL = false;
	conf->endpointOverride = "127.0.0.1:9000";
}

void *
S3InitAccess()
{
	auto *s3client = new S3Access();

	s3client->op = new Aws::SDKOptions();
	InitAPI(*s3client->op);
	ClientConfiguration conf;
	SetCofig(&conf);

	s3client->cli = new S3Client(AWSCredentials("minioadmin", "minioadmin"),
								   conf,
								   AWSAuthV4Signer::PayloadSigningPolicy::Never,
								   false);

	return s3client;
}

void
S3DestroyAccess(void *s3Client)
{
	S3Access *s3_client = static_cast<S3Access *>(s3Client);

	delete s3_client->cli;
	ShutdownAPI(*s3_client->op);
}

void
S3DeleteBucket(void *s3Client, const char *bucketPath)
{
	Model::DeleteBucketRequest req;
	S3Access *s3_client = static_cast<S3Access *>(s3Client);

	req.SetBucket(bucketPath);
	Model::DeleteBucketOutcome result = s3_client->cli->DeleteBucket(req);

	if (!result.IsSuccess())
	{
		printf("DeleteBucket failed with error '%s'",
					 result.GetError().GetMessage().c_str());
		exit(0);
	}
}


S3Objs
S3DeleteObjects(void *s3Client, const char *prefix)
{
	Model::ListObjectsV2Request req;
	S3Objs s3_obj;
	S3Access *s3_client = static_cast<S3Access *>(s3Client);

	s3_obj.objPathList = NIL;
	req.WithBucket(default_bucket_name);
	if (prefix)
		req.WithPrefix(prefix);

	Aws::String continuationToken; // Used for pagination.

	do
	{
		if (!continuationToken.empty()) {
			req.SetContinuationToken(continuationToken);
		}
		auto result = s3_client->cli->ListObjectsV2(req);

		if (!result.IsSuccess())
		{
			printf("ListObject failed with error '%s'",
				   result.GetError().GetMessage().c_str());
			exit(0);
		}

		Aws::Vector<Model::Object> objList = result.GetResult().GetContents();

		continuationToken = result.GetResult().GetNextContinuationToken();
		for (Model::Object& obj : objList)
		{
			S3DeleteObject(s3Client, obj.GetKey().c_str());
		}
	} while (!continuationToken.empty());

	return s3_obj;
}

void
S3DeleteObject(void *s3Client, const char *objPath)
{
	Model::DeleteObjectRequest req;

	req.WithKey(objPath).WithBucket(default_bucket_name);
	S3Access *s3_client = static_cast<S3Access *>(s3Client);

	Model::DeleteObjectOutcome result = s3_client->cli->DeleteObject(req);

	if (!result.IsSuccess())
	{
		printf("DeleteObject failed with error '%s'",
			   result.GetError().GetMessage().c_str());
		exit(0);
	}
}


