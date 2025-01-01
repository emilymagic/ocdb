#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/DeleteBucketRequest.h>
#include <aws/s3/model/ListObjectsRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadBucketRequest.h>

extern "C" {
#include "postgres.h"
}

#include "storage/objectfilerw.h"
using namespace Aws::S3;
using namespace Aws::Client;
using namespace Aws::Auth;

typedef struct S3Access
{
	S3Client *cli;
	Aws::SDKOptions *op;
} S3Access;

const char *default_bucket_name = "dbdata1";

static void  SetCofig(ClientConfiguration *conf);

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

	return static_cast<void*>(s3client);
}

void
S3DestroyAccess(void *s3Client)
{
	S3Access *s3_client = static_cast<S3Access *>(s3Client);

	delete s3_client->cli;
	ShutdownAPI(*s3_client->op);
}

void
S3CreateBucket(void *s3Client, const char *bucketPath)
{
	Model::CreateBucketRequest req;
	S3Access *s3_client = static_cast<S3Access *>(s3Client);

	req.SetBucket(bucketPath);
	Model::CreateBucketOutcome result = s3_client->cli->CreateBucket(req);

	if (!result.IsSuccess()) {
		auto err = result.GetError();
		elog(ERROR, "CreateBucket failed with error '%s'",err.GetMessage().c_str());
	}
}

void
S3DeleteBucket(void *s3Client, const char *bucketPath)
{
	Model::DeleteBucketRequest req;
	S3Access *s3_client = static_cast<S3Access *>(s3Client);

	req.SetBucket(bucketPath);
	Model::DeleteBucketOutcome result = s3_client->cli->DeleteBucket(req);

	if (!result.IsSuccess()) {
		auto err = result.GetError();
		elog(ERROR, "DeleteBucket failed with error '%s'", err.GetMessage().c_str());
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
	req.WithPrefix(prefix);

	Aws::String continuationToken; // Used for pagination.

	do
	{
		if (!continuationToken.empty()) {
			req.SetContinuationToken(continuationToken);
		}
		auto result = s3_client->cli->ListObjectsV2(req);

		if (!result.IsSuccess())
			elog(ERROR, "ListObject failed with error '%s'",
				 result.GetError().GetMessage().c_str());

		Aws::Vector<Model::Object> objList = result.GetResult().GetContents();

		continuationToken = result.GetResult().GetNextContinuationToken();
		for (Model::Object& obj : objList)
		{
			char *objPath;
			objPath = static_cast<char *>(palloc(obj.GetKey().length() + 1));
			strcpy(objPath, obj.GetKey().c_str());
			s3_obj.objPathList = lappend(s3_obj.objPathList, objPath);
		}
	} while (!continuationToken.empty());

	return s3_obj;
}

S3Obj
S3GetObject(void *s3Client, S3ObjKey s3_obj_key)
{
	Model::GetObjectRequest req;

	S3Obj s3_obj;
	S3Access *s3_client = static_cast<S3Access *>(s3Client);

	s3_obj.data = nullptr;
	s3_obj.size = 0;

	char *name = static_cast<char *>
		(palloc(strlen(s3_obj_key.bucketName) + strlen(s3_obj_key.objectName) + 2));
	sprintf(name, "%s_%s", s3_obj_key.bucketName, s3_obj_key.objectName);
	req.SetBucket(default_bucket_name);
	req.SetKey(name);
	pfree(name);
	Model::GetObjectOutcome result = s3_client->cli->GetObject(req);

	if (!result.IsSuccess())
	{
		auto err = result.GetError();
		elog(ERROR, "GetObject failed with error '%s'", err.GetMessage().c_str());
	}

	auto& body = result.GetResultWithOwnership().GetBody();
	s3_obj.size = result.GetResultWithOwnership().GetContentLength();
	body.read(s3_obj.data, s3_obj.size);

	return s3_obj;
}

uint32
S3GetObject2(void *s3Client, const char *bucketPath, const char *objPath,
			 char *data)
{
	Model::GetObjectRequest req;
	uint32 dataSize = 0;
	S3Access *s3_client = static_cast<S3Access *>(s3Client);

	char *name = static_cast<char *> (palloc(strlen(bucketPath) + strlen(objPath) + 2));
	sprintf(name, "%s_%s", bucketPath, objPath);

	req.SetBucket(default_bucket_name);
	req.SetKey(name);
	pfree(name);
	Model::GetObjectOutcome result = s3_client->cli->GetObject(req);

	if (!result.IsSuccess())
	{
		auto err = result.GetError();
		elog(ERROR, "GetObject failed with error '%s'", err.GetMessage().c_str());
	}

	auto& body = result.GetResultWithOwnership().GetBody();
	dataSize = result.GetResultWithOwnership().GetContentLength();

	body.read(data, dataSize);

	return dataSize;
}

void
S3PutObject(void *s3Client, S3ObjKey s3_obj_key, S3Obj s3_obj)
{
	Model::PutObjectRequest req;
	char *name = static_cast<char *>
		(palloc(strlen(s3_obj_key.bucketName) + strlen(s3_obj_key.objectName) + 2));
	sprintf(name, "%s_%s", s3_obj_key.bucketName, s3_obj_key.objectName);
	req.SetBucket(default_bucket_name);
	req.SetKey(name);
	pfree(name);
	S3Access *s3_client = static_cast<S3Access *>(s3Client);

	auto writer = Aws::MakeShared<Aws::StringStream>(
			"PutObjectInputStream",
			std::stringstream::in | std::stringstream::out | std::stringstream::binary);
	writer->write(s3_obj.data, s3_obj.size);

	req.SetBody(writer);
	Model::PutObjectOutcome result = s3_client->cli->PutObject(req);

	if (!result.IsSuccess())
	{
		auto err = result.GetError();
		elog(ERROR, "PutObject failed with error '%s'", err.GetMessage().c_str());
	}
}

void
S3DeleteObject(void *s3Client, char *objPath)
{
	Model::DeleteObjectRequest req;

	req.WithKey(objPath).WithBucket(default_bucket_name);
	S3Access *s3_client = static_cast<S3Access *>(s3Client);

	Model::DeleteObjectOutcome result = s3_client->cli->DeleteObject(req);

	if (!result.IsSuccess())
	{
		auto err = result.GetError();
		elog(ERROR, "DeleteObject failed with error '%s'", err.GetMessage().c_str());
	}
}

bool
S3BucketExist(void *s3Client, const char *bucketName)
{
	S3Access *s3_client = static_cast<S3Access *>(s3Client);
	Model::HeadBucketRequest req;

	req.SetBucket(bucketName);
	Model::HeadBucketOutcome result = s3_client->cli->HeadBucket(req);

	return result.IsSuccess();
}

static void
SetCofig(ClientConfiguration *conf)
{
	conf->region = "us-east-1";
	conf->scheme = Aws::Http::Scheme::HTTP;
	conf->verifySSL = false;
	conf->endpointOverride = "127.0.0.1:9000";
}
