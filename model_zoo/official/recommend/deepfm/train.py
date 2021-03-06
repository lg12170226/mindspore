# Copyright 2020 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ============================================================================
"""train_criteo."""
import os
import sys
import argparse
import random
import numpy as np

from mindspore import context, ParallelMode
from mindspore.communication.management import init, get_rank, get_group_size
from mindspore.train.model import Model
from mindspore.train.callback import ModelCheckpoint, CheckpointConfig, TimeMonitor
import mindspore.dataset.engine as de

from src.deepfm import ModelBuilder, AUCMetric
from src.config import DataConfig, ModelConfig, TrainConfig
from src.dataset import create_dataset, DataType
from src.callback import EvalCallBack, LossCallBack

sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
parser = argparse.ArgumentParser(description='CTR Prediction')
parser.add_argument('--dataset_path', type=str, default=None, help='Dataset path')
parser.add_argument('--ckpt_path', type=str, default=None, help='Checkpoint path')
parser.add_argument('--eval_file_name', type=str, default="./auc.log", help='eval file path')
parser.add_argument('--loss_file_name', type=str, default="./loss.log", help='loss file path')
parser.add_argument('--do_eval', type=bool, default=True, help='Do evaluation or not.')
parser.add_argument('--device_target', type=str, default="Ascend", help='Ascend, GPU, or CPU')
args_opt, _ = parser.parse_known_args()
rank_size = int(os.environ.get("RANK_SIZE", 1))

random.seed(1)
np.random.seed(1)
de.config.set_seed(1)

if __name__ == '__main__':
    data_config = DataConfig()
    model_config = ModelConfig()
    train_config = TrainConfig()

    if rank_size > 1:
        if args_opt.device_target == "Ascend":
            device_id = int(os.getenv('DEVICE_ID'))
            context.set_context(mode=context.GRAPH_MODE, device_target=args_opt.device_target, device_id=device_id)
            context.reset_auto_parallel_context()
            context.set_auto_parallel_context(parallel_mode=ParallelMode.DATA_PARALLEL, mirror_mean=True)
            init()
            rank_id = int(os.environ.get('RANK_ID'))
        elif args_opt.device_target == "GPU":
            init("nccl")
            context.set_context(mode=context.GRAPH_MODE, device_target=args_opt.device_target)
            context.reset_auto_parallel_context()
            context.set_auto_parallel_context(device_num=get_group_size(),
                                              parallel_mode=ParallelMode.DATA_PARALLEL,
                                              mirror_mean=True)
            rank_id = get_rank()
        else:
            print("Unsupported device_target ", args_opt.device_target)
            exit()
    else:
        device_id = int(os.getenv('DEVICE_ID'))
        context.set_context(mode=context.GRAPH_MODE, device_target=args_opt.device_target, device_id=device_id)
        rank_size = None
        rank_id = None

    ds_train = create_dataset(args_opt.dataset_path,
                              train_mode=True,
                              epochs=1,
                              batch_size=train_config.batch_size,
                              data_type=DataType(data_config.data_format),
                              rank_size=rank_size,
                              rank_id=rank_id)

    steps_size = ds_train.get_dataset_size()

    model_builder = ModelBuilder(ModelConfig, TrainConfig)
    train_net, eval_net = model_builder.get_train_eval_net()
    auc_metric = AUCMetric()
    model = Model(train_net, eval_network=eval_net, metrics={"auc": auc_metric})

    time_callback = TimeMonitor(data_size=ds_train.get_dataset_size())
    loss_callback = LossCallBack(loss_file_path=args_opt.loss_file_name)
    callback_list = [time_callback, loss_callback]

    if train_config.save_checkpoint:
        if rank_size:
            train_config.ckpt_file_name_prefix = train_config.ckpt_file_name_prefix + str(get_rank())
        if args_opt.device_target == "GPU":
            config_ck = CheckpointConfig(save_checkpoint_steps=steps_size,
                                         keep_checkpoint_max=train_config.keep_checkpoint_max)
        else:
            config_ck = CheckpointConfig(save_checkpoint_steps=train_config.save_checkpoint_steps,
                                         keep_checkpoint_max=train_config.keep_checkpoint_max)
        ckpt_cb = ModelCheckpoint(prefix=train_config.ckpt_file_name_prefix,
                                  directory=args_opt.ckpt_path,
                                  config=config_ck)
        callback_list.append(ckpt_cb)

    if args_opt.do_eval:
        ds_eval = create_dataset(args_opt.dataset_path, train_mode=False,
                                 epochs=1,
                                 batch_size=train_config.batch_size,
                                 data_type=DataType(data_config.data_format))
        eval_callback = EvalCallBack(model, ds_eval, auc_metric,
                                     eval_file_path=args_opt.eval_file_name)
        callback_list.append(eval_callback)
    model.train(train_config.train_epochs, ds_train, callbacks=callback_list)
