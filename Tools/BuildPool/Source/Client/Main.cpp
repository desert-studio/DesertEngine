// The buildpool executable: launcher, Back and daemon in one binary (BuildPool/Invocation.hpp RunClient).
#include "BuildPool/Invocation.hpp"

int main( int argc, char** argv )
{
    return buildpool::RunClient( argc, argv );
}
