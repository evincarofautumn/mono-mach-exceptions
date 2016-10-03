native_library=libexception-negotiation-native.dylib
native_source=exception-negotiation-native.c
managed_executable=exception-negotiation.exe
managed_source=exception-negotiation.cs

all : $(native_library) $(managed_executable)

$(native_library) : $(native_source)
	$(CC) $(native_source) -shared -o $(native_library) -arch i386

$(managed_executable) : $(managed_source)
	mcs $(managed_source)
