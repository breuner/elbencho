#ifndef TOOLKITS_S3TK_H_
#define TOOLKITS_S3TK_H_

#include "ProgArgs.h"

#ifdef S3_SUPPORT
	#include <aws/core/Aws.h>
#endif

class S3Tk
{
	public:
		static void initS3Global(const ProgArgs& progArgs);
		static void uninitS3Global(const ProgArgs& progArgs);

	private:

	#ifdef S3_SUPPORT
		static bool globalInitCalled; // to make uninit a no-op if init wasn't called

		static Aws::SDKOptions* s3SDKOptions;
	#endif
};

#endif /* TOOLKITS_S3TK_H_ */
