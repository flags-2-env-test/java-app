FROM eclipse-temurin:21-jdk

WORKDIR /app

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential make \
 && rm -rf /var/lib/apt/lists/*

COPY .vendor/.zed/oresoftware/flags-2-env ./.vendor/.zed/oresoftware/flags-2-env

# Java does not use the shared library the other clients load. It binds through
# a JNI shim, so the native artifact built here is libflags2env_jni.so and the
# lookup happens via java.library.path rather than FLAGS2ENV_NATIVE_LIB.
RUN mkdir -p /app/native \
 && cc -std=c99 -fPIC -shared \
      -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
      -I.vendor/.zed/oresoftware/flags-2-env/clients/java/native \
      .vendor/.zed/oresoftware/flags-2-env/clients/java/native/flags2env_jni.c \
      .vendor/.zed/oresoftware/flags-2-env/clients/java/native/parser.c \
      -o /app/native/libflags2env_jni.so

COPY .cli-flags.toml ./
COPY src ./src

RUN javac -d /tmp/classes \
      .vendor/.zed/oresoftware/flags-2-env/clients/java/src/main/java/com/oresoftware/flags2env/Flags2Env.java \
      src/main/java/test/flags2env/Demo.java

CMD ["java", "-Djava.library.path=/app/native", "-cp", "/tmp/classes", "test.flags2env.Demo"]
