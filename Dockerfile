# 基于 Ubuntu 22.04
FROM ubuntu:22.04

# 设置环境变量，避免交互式提示
ENV DEBIAN_FRONTEND=noninteractive

# 更新软件源并安装 Python 3.10 及 pip
RUN apt-get update && \
    apt-get install -y python3.10 python3-pip && \
    # 创建软链接，方便使用 python 和 pip 命令
    ln -sf /usr/bin/python3.10 /usr/bin/python && \
    ln -sf /usr/bin/pip3 /usr/bin/pip && \
    # 清理缓存，减小镜像体积
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# 验证安装
RUN python --version && pip --version

# 设置默认工作目录
WORKDIR /app

# 默认启动 bash
CMD ["/bin/bash"]