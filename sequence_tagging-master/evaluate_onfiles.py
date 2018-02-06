from model.data_utils import CoNLLDataset
from model.ner_model import NERModel
from model.config import Config
from model.data_utils import minibatches, pad_sequences, get_chunks
import copy
import tensorflow as tf
def load(filename):
    with open(filename, 'r', encoding="utf8") as f:
        lines = f.readlines()
    temp = []
    data = []
    cpt = 0
    for line_ in lines:
        if line_ == '\n':
            #print('line')
            cpt +=1
        if cpt == 0:
            #print('aaa')
            temp.append(line_.split()[0])
        elif cpt == 2:
            cpt = 0
            data.append(temp)
            temp = []
    
    return data

def align_data(data):
    """Given dict with lists, creates aligned strings

    Adapted from Assignment 3 of CS224N

    Args:
        data: (dict) data["x"] = ["I", "love", "you"]
              (dict) data["y"] = ["O", "O", "O"]

    Returns:
        data_aligned: (dict) data_align["x"] = "I love you"
                           data_align["y"] = "O O    O  "

    """
    spacings = [max([len(seq[i]) for seq in data.values()])
                for i in range(len(data[list(data.keys())[0]]))]
    data_aligned = dict()

    # for each entry, create aligned string
    for key, seq in data.items():
        str_aligned = ""
        for token, spacing in zip(seq, spacings):
            str_aligned += token + " " * (spacing - len(token) + 1)

        data_aligned[key] = str_aligned

    return data_aligned



def interactive_shell(model):
    """Creates interactive shell to play with model

    Args:
        model: instance of NERModel

    """
    model.logger.info("""
This is an interactive mode.
To exit, enter 'exit'.
You can enter a sentence like
input> I love Paris""")

    while True:
        try:
            # for python 2
            sentence = raw_input("input> ")
        except NameError:
            # for python 3
            sentence = input("input> ")

        words_raw = sentence.strip().split(" ")

        if words_raw == ["exit"]:
            break

        preds = model.predict(words_raw)
        to_print = align_data({"input": words_raw, "output": preds})

        for key, seq in to_print.items():
            model.logger.info(seq)

def write_pred(datas, labels, name):
    with open(name, 'w', encoding='utf8') as f:
        for i, line in enumerate(labels):
            for j, word in enumerate(line):
                f.write(datas[i][j] + ' ' + word.lower() + '\n')
            f.write('\n\n')
            
def main():
    '''
        evaluate using saved models
    '''
    # create instance of config
    config = Config()

    # build model
    model = NERModel(config)
    model.build()
    model.restore_session(config.dir_model)

    # create dataset
    test  = CoNLLDataset(config.filename_test, config.processing_word,
                         config.processing_tag, config.max_iter)
    print(type(config.vocab_words))
    # eval uate and interact
    #model.evaluate(test)
    lab = []
    seqs = []
    for words, labels in minibatches(test, 1):
            temp = []
            temp2 = []
            w = copy.deepcopy(words)
            A = list(w[0])
                        
            labels_pred, sequence_lengths = model.predict_batch(words)
            #fd, sequence_lengths = model.get_feed_dict(words, dropout=1.0)
            
            
            
            for i, y in enumerate(labels_pred[0]):
                x = A[0][i]
                temp3 = []
                for letter in x:
                    #print(letter)
                    temp3.append(model.idx_to_char[letter])
                temp.append(model.idx_to_tag[y])
                temp2.append(''.join(temp3))
                #temp2.append(model.config.processing_word[x])
            lab.append(temp)
            seqs.append(temp2)
    print(lab[0:3])        
    print(seqs[0:3])
    #interactive_shell(model)
    name = 'pred_give_ingredient_dev.txt'
    data = load(config.filename_test)
    print(data[0:3])
    write_pred(data, lab, name)
        
if __name__ == "__main__":
    with tf.device('/cpu:0'):  
        main()
