#ifndef COMMON_H_
#define COMMON_H_

#define PHASENAME_IDLE			"IDLE"
#define PHASENAME_TERMINATE		"QUIT"
#define PHASENAME_CREATEDIRS	"MKDIRS"
#define PHASENAME_CREATEFILES	"MKFILES"
#define PHASENAME_READFILES		"READ"
#define PHASENAME_DELETEFILES	"RMFILES"
#define PHASENAME_DELETEDIRS	"RMDIRS"


#define PHASEENTRYTYPE_DIRS		"dirs"
#define PHASEENTRYTYPE_FILES	"files"

#define HTTP_PROTOCOLVERSION	"1.0.0" // exchanged between client & server to check compatibility

enum BenchPhase
{
	BenchPhase_IDLE = 0,
	BenchPhase_TERMINATE, // tells workers to self-terminate when all is done
	BenchPhase_CREATEDIRS,
	BenchPhase_DELETEDIRS,
	BenchPhase_CREATEFILES,
	BenchPhase_DELETEFILES,
	BenchPhase_READFILES,
};


#endif /* COMMON_H_ */
