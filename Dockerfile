# Dockerfile for ELK stack
# Elasticsearch, Logstash, Kibana 7.4.0

# Build with:
# docker build -t <repo-user>/elk .

# Run with:
# docker run -p 5601:5601 -p 9200:9200 -p 5044:5044 -it --name elk <repo-user>/elk

FROM hyp3rsonix/ubuntu-1810
MAINTAINER M. Emre Akkus
ENV \
 REFRESHED_AT=2019-11-14


###############################################################################
#                                INSTALLATION
###############################################################################

### install prerequisites (cURL, gosu, JDK, tzdata)

RUN set -x \
 && apt update -qq \
 && apt install -qqy --no-install-recommends ca-certificates curl gosu tzdata openjdk-8-jdk zip unzip \
 && apt clean \
 && rm -rf /var/lib/apt/lists/* \
 && gosu nobody true \
 && set +x

### install Elasticsearch
ARG ELK_VERSION=7.4.2
ENV \
 ES_VERSION=${ELK_VERSION} \
 ES_HOME=/opt/elasticsearch \
 LOGSTASH_VERSION=${ELK_VERSION} \
 LOGSTASH_HOME=/opt/logstash

# note you can't define an env var that references another one in the same block (docker layer)
ENV \
 JAVA_HOME=/usr/lib/jvm/java-8-openjdk-arm64/jre \
 ES_PACKAGE=elasticsearch-${ES_VERSION}-no-jdk-linux-x86_64.tar.gz \
 ES_GID=991 \
 ES_UID=991 \
 ES_PATH_CONF=/etc/elasticsearch \
 ES_PATH_BACKUP=/var/backups \
 KIBANA_VERSION=${ELK_VERSION}

RUN echo "${ELK_VERSION} ${ES_VERSION} https://artifacts.elastic.co/downloads/elasticsearch/${ES_PACKAGE} to ${ES_HOME}"
RUN DEBIAN_FRONTEND=noninteractive \
 && mkdir ${ES_HOME} \
 && curl -O https://artifacts.elastic.co/downloads/elasticsearch/${ES_PACKAGE} \
 && tar xzf ${ES_PACKAGE} -C ${ES_HOME} --strip-components=1 \
 && rm -f ${ES_PACKAGE} \
 && groupadd -r elasticsearch -g ${ES_GID} \
 && useradd -r -s /usr/sbin/nologin -M -c "Elasticsearch service user" -u ${ES_UID} -g elasticsearch elasticsearch \
 && mkdir -p /var/log/elasticsearch ${ES_PATH_CONF} ${ES_PATH_CONF}/scripts /var/lib/elasticsearch ${ES_PATH_BACKUP} \
 && chown -R elasticsearch:elasticsearch ${ES_HOME} /var/log/elasticsearch /var/lib/elasticsearch ${ES_PATH_CONF} ${ES_PATH_BACKUP}


### install Logstash

ENV \
 LOGSTASH_PACKAGE=logstash-${LOGSTASH_VERSION}.tar.gz \
 LOGSTASH_GID=992 \
 LOGSTASH_UID=992 \
 LOGSTASH_PATH_CONF=/etc/logstash \
 LOGSTASH_PATH_SETTINGS=${LOGSTASH_HOME}/config

RUN mkdir ${LOGSTASH_HOME} \
 && curl -O https://artifacts.elastic.co/downloads/logstash/${LOGSTASH_PACKAGE} \
 && tar xzf ${LOGSTASH_PACKAGE} -C ${LOGSTASH_HOME} --strip-components=1 \
 && rm -f ${LOGSTASH_PACKAGE} \
 && groupadd -r logstash -g ${LOGSTASH_GID} \
 && useradd -r -s /usr/sbin/nologin -d ${LOGSTASH_HOME} -c "Logstash service user" -u ${LOGSTASH_UID} -g logstash logstash \
 && mkdir -p /var/log/logstash ${LOGSTASH_PATH_CONF}/conf.d \
 && chown -R logstash:logstash ${LOGSTASH_HOME} /var/log/logstash ${LOGSTASH_PATH_CONF} \
 && mkdir -p /tmp/jruby-complete \
 && unzip -q /opt/logstash/logstash-core/lib/jars/jruby-complete-9.2.8.0.jar -d /tmp/jruby-complete \
 && cp /tmp/jruby-complete/META-INF/jruby.home/lib/ruby/stdlib/ffi/platform/aarch64-linux/types.conf /tmp/jruby-complete/META-INF/jruby.home/lib/ruby/stdlib/ffi/platform/aarch64-linux/platform.conf \
 && cd /tmp/jruby-complete \
 && zip -qr /tmp/jruby-complete-9.2.8.0.jar * \
 && cp -f /tmp/jruby-complete-9.2.8.0.jar /opt/logstash/logstash-core/lib/jars/jruby-complete-9.2.8.0.jar


### install Kibana

ADD ./nodegit /temp/nodegit

ENV \
 KIBANA_HOME=/opt/kibana \
 KIBANA_PACKAGE=kibana-${KIBANA_VERSION}-linux-x86_64.tar.gz \
 KIBANA_GID=993 \
 KIBANA_UID=993

RUN mkdir ${KIBANA_HOME} \
 && curl -O https://artifacts.elastic.co/downloads/kibana/${KIBANA_PACKAGE} \
 && tar xzf ${KIBANA_PACKAGE} -C ${KIBANA_HOME} --strip-components=1 \
 && rm -f ${KIBANA_PACKAGE} \
 && groupadd -r kibana -g ${KIBANA_GID} \
 && useradd -r -s /usr/sbin/nologin -d ${KIBANA_HOME} -c "Kibana service user" -u ${KIBANA_UID} -g kibana kibana \
 && mkdir -p /var/log/kibana \
 && chown -R kibana:kibana ${KIBANA_HOME} /var/log/kibana \
 && rm -rf /opt/kibana/node/* \
 && curl -sL https://nodejs.org/dist/v10.15.2/node-v10.15.2-linux-arm64.tar.gz | tar -C /opt/kibana/node/ --strip-components=1 -xzf - \
 && chown -R kibana:kibana ${KIBANA_HOME} /opt/kibana/node \
 && chmod -R 777 /temp/nodegit \
 && cp -rf /temp/nodegit/build/Release /opt/kibana/node_modules/@elastic/nodegit/build \
 && cp /temp/nodegit/dist/enums.js /opt/kibana/node_modules/@elastic/nodegit/dist \
 && mv /opt/kibana/node_modules/@elastic/node-ctags/ctags/build/ctags-node-v64-linux-x64 /opt/kibana/node_modules/@elastic/node-ctags/ctags/build/ctags-node-v64-linux-arm64 \
 && cp /temp/nodegit/node_modules/ctags/build/Release/ctags.node /opt/kibana/node_modules/@elastic/node-ctags/ctags/build/ctags-node-v64-linux-arm64 \
 && rm -rf /temp/nodegit


###############################################################################
#                              START-UP SCRIPTS
###############################################################################

### Elasticsearch

ADD ./elasticsearch-init /etc/init.d/elasticsearch
RUN sed -i -e 's#^ES_HOME=$#ES_HOME='$ES_HOME'#' /etc/init.d/elasticsearch \
 && chmod +x /etc/init.d/elasticsearch

### Logstash

ADD ./logstash-init /etc/init.d/logstash
RUN sed -i -e 's#^LS_HOME=$#LS_HOME='$LOGSTASH_HOME'#' /etc/init.d/logstash \
 && chmod +x /etc/init.d/logstash

### Kibana

ADD ./kibana-init /etc/init.d/kibana
RUN sed -i -e 's#^KIBANA_HOME=$#KIBANA_HOME='$KIBANA_HOME'#' /etc/init.d/kibana \
 && chmod +x /etc/init.d/kibana


###############################################################################
#                               CONFIGURATION
###############################################################################

### configure Elasticsearch

ADD ./elasticsearch.yml ${ES_PATH_CONF}/elasticsearch.yml
ADD ./elasticsearch-default /etc/default/elasticsearch
RUN cp ${ES_HOME}/config/log4j2.properties ${ES_HOME}/config/jvm.options \
    ${ES_PATH_CONF} \
 && chown -R elasticsearch:elasticsearch ${ES_PATH_CONF} \
 && chmod -R +r ${ES_PATH_CONF}

### configure Logstash

# certs/keys for Beats and Lumberjack input
RUN mkdir -p /etc/pki/tls/{certs,private}
ADD ./logstash-beats.crt /etc/pki/tls/certs/logstash-beats.crt
ADD ./logstash-beats.key /etc/pki/tls/private/logstash-beats.key

# pipelines
ADD pipelines.yml ${LOGSTASH_PATH_SETTINGS}/pipelines.yml

# filters
ADD ./logstash-conf/*.conf ${LOGSTASH_PATH_CONF}/conf.d/

# patterns
ADD ./nginx.pattern ${LOGSTASH_HOME}/patterns/nginx
RUN chown -R logstash:logstash ${LOGSTASH_HOME}/patterns

# Fix permissions
RUN chmod -R +r ${LOGSTASH_PATH_CONF} ${LOGSTASH_PATH_SETTINGS} \
 && chown -R logstash:logstash ${LOGSTASH_PATH_SETTINGS}

### configure logrotate

ADD ./elasticsearch-logrotate /etc/logrotate.d/elasticsearch
ADD ./logstash-logrotate /etc/logrotate.d/logstash
ADD ./kibana-logrotate /etc/logrotate.d/kibana
RUN chmod 644 /etc/logrotate.d/elasticsearch \
 && chmod 644 /etc/logrotate.d/logstash \
 && chmod 644 /etc/logrotate.d/kibana


### configure Kibana

ADD ./kibana.yml ${KIBANA_HOME}/config/kibana.yml


###############################################################################
#                                   START
###############################################################################

ADD ./start.sh /usr/local/bin/start.sh
RUN chmod +x /usr/local/bin/start.sh

EXPOSE 5601 9200 9300 5044
VOLUME /var/lib/elasticsearch

CMD [ "/usr/local/bin/start.sh" ]
