#include <exception>
#include "Coordinator.h"
#include "Logger.h"
#include "ProgArgs.h"

/**
 * Parse command line args, check if we just need to print help and otherwise leave the rest to the
 * Coordinator class.
 */
int main(int argc, char** argv)
{
	try
	{
		ProgArgs progArgs(argc, argv);

		if(progArgs.hasUserRequestedHelp() )
		{
			progArgs.printHelp();
			return EXIT_SUCCESS;
		}

		if(progArgs.hasUserRequestedVersion() )
		{
			progArgs.printVersionAndBuildInfo();
			return EXIT_SUCCESS;
		}

		time_t waittimeSec = progArgs.getStartTime() ? progArgs.getStartTime() - time(NULL) : 0;

		// print original command line

		Logger cmdLog(Log_VERBOSE);
		cmdLog << "COMMAND LINE: ";
		for(int i=0; i < argc; i++)
			cmdLog << "\"" << argv[i] << "\" ";

		cmdLog << std::endl;
		cmdLog.flush();

		// print parsed command line arg values

		LOGGER(Log_DEBUG, "CONFIG VALUES: " <<
			"progPath: " << progArgs.getProgPath() << "; " <<
			"threads: " << progArgs.getNumThreads() << "; " <<
			"dirs: " << progArgs.getNumDirs() << "; " <<
			"files: " << progArgs.getNumFiles() << "; "
			"benchPath: " << progArgs.getBenchPathStr() << "; " <<
			"hosts: " << progArgs.getHostsStr() << "; " <<
			"mkdi: " << progArgs.getRunCreateDirsPhase() << "; " <<
			"mkfi: " << progArgs.getRunCreateFilesPhase() << "; " <<
			"read: " << progArgs.getRunReadPhase() << "; " <<
			"rmfi: " << progArgs.getRunDeleteFilesPhase() << "; " <<
			"rmdi: " << progArgs.getRunDeleteDirsPhase() << "; " <<
			"lsobjp: " << progArgs.getRunListObjParallelPhase() << "; " <<
			"starttime: " << progArgs.getStartTime() << "; " <<
			"waittime: " << waittimeSec << std::endl);

		return Coordinator(progArgs).main();
	}
	catch(std::exception& e)
	{
		ErrLogger() << e.what() << std::endl;
		return EXIT_FAILURE;
	}
}
