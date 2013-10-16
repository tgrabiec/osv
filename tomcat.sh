#!/usr/bin/env bash

#
# TOMCAT @ OSV
#

CATALINA_BASE=/usr/tomcat
CATALINA_HOME=$CATALINA_BASE

if [ -z "$JAVA_ENDORSED_DIRS" ]; then
  JAVA_ENDORSED_DIRS="$CATALINA_HOME"/endorsed
fi

# Set juli LogManager config file if it is present and an override has not been issued
if [ -z "$LOGGING_CONFIG" ]; then
  LOGGING_CONFIG="-Djava.util.logging.config.file=$CATALINA_BASE/conf/logging.properties"
fi

if [ -z "$LOGGING_MANAGER" ]; then
  LOGGING_MANAGER="-Djava.util.logging.manager=org.apache.juli.ClassLoaderLogManager"
fi

# Add on extra jar files to CLASSPATH
if [ ! -z "$CLASSPATH" ] ; then
  CLASSPATH="$CLASSPATH":
fi
CLASSPATH="$CLASSPATH""$CATALINA_HOME"/bin/bootstrap.jar

if [ -z "$CATALINA_OUT" ] ; then
  CATALINA_OUT="$CATALINA_BASE"/logs/catalina.out
fi

if [ -z "$CATALINA_TMPDIR" ] ; then
  # Define the java.io.tmpdir to use for Catalina
  CATALINA_TMPDIR="$CATALINA_BASE"/temp
fi

CLASSPATH=$CLASSPATH:$CATALINA_HOME/bin/tomcat-juli.jar

OSV_OPTS="-n -v"
LOADER_ARGS=
DEBUG_OPTS=

while [ -n "$1" ]; do
  case $1 in
    "--suspend")
      DEBUG_OPTS="-agentlib:jdwp=transport=dt_socket,server=y,suspend=y,address=5005"
      ;;
    "--debug")
      OSV_OPTS="$OSV_OPTS -d"
      ;;
    "--loader")
      shift
      LOADER_ARGS="$LOADER_ARGS $1"
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
  shift
done

CMD="$LOADER_ARGS java.so $DEBUG_OPTS -cp $CLASSPATH \
       $LOGGING_CONFIG $LOGGING_MANAGER \
       $JAVA_OPTS $CATALINA_OPTS \
       -Djava.endorsed.dirs=$JAVA_ENDORSED_DIRS \
       -Dcatalina.base=$CATALINA_BASE \
       -Dcatalina.home=$CATALINA_HOME \
       -Djava.io.tmpdir=$CATALINA_TMPDIR \
       org.apache.catalina.startup.Bootstrap start"
      
echo Running: $CMD

scripts/run.py $OSV_OPTS -e "$CMD"
