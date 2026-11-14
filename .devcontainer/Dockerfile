# 项目依赖 ESP-IDF v6.1 的 I2S 实现，使用官方发布标签固定 SDK 版本。
# 不使用跟随 master 的 latest；升级前须核对 main/CMakeLists.txt 中的 DMA 钩子。
FROM espressif/idf:v6.1

ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

RUN apt-get update -y && apt-get install udev -y

RUN echo "source /opt/esp/idf/export.sh > /dev/null 2>&1" >> ~/.bashrc

ENTRYPOINT [ "/opt/esp/entrypoint.sh" ]

CMD ["/bin/bash", "-c"]
