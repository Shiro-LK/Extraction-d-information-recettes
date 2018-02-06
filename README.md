# Extraction-d-information-recettes
Projet d'extraction d'information recette de cuisine

Le fichier 
Les Fichiers de prédictions se trouvent dans eval-nlu/bydataset et commence par "pred_" sous le format .xml
Les Fichiers de dev/test se trouvent également dans eval-nlu/bydataset sous le format .xml

# entrainement : dossier => sequence_tagging_master

Le modèle d'entrainement provient du github https://github.com/guillaumegenthial/sequence_tagging que nous avons légèrement modifier. Nous avons notamment ajouté le fichier evaluate_onfiles.py" permettant de sauvegarder les prédictions du test dans un format similaire au .bio . Il suffit ensuite de le convertir en .xml et utiliser le programme d'evaluation fournis présent dans eval-nlu.

Les fichiers utilisés pour entrainer, valider (dev) et tester les résultats sont respectivement dans : 
- sequence_tagging_master/data/train avec les fichiers give_cat-ingredients-trn_train.txt, give_ingredient_tr.txt, recipe_tr20k.txt pour l'apprentissage par type de classe et tr_20k.txt pour l'apprentissage de l'ensemble des classes.
- sequence_tagging_master/data/dev avec les fichiers : give_ingredient_dev.txt, give_cat_dev.txt , recipe_dev.txt pour l'apprentissage par type de classe et all_data_dev.txt pour l'apprentissage de l'ensemble des classes.
- sequence_tagging_master/data/test avec les fichiers give_ingredient_test.txt, recipe_test.txt, give_cat_test.txt pour l'apprentissage par type de classe et all_data_test.txt pour l'apprentissage de l'ensemble des classes.

## configuration entrainement
- sequence_tagging_master/data/create_dataset.py : pour entrainer les fichiers, les labels doivent etre en lettre majuscule or de base, nos labels sont en minuscules. 
Ce fichier permet donc de convertir nos fichiers d'entrainement, dev et test afin de les adapter pour l'entrainement du réseau de neurones.

- sequence_tagging_master/data/model/config.py : permet de configurer l'entrainement des réseaux de neurones (batch size, learning rate, nom des fichiers d'entrainements etc)
- sequence_tagging_master/data/model/build_data.py : a lancer avant d'entrainer

- sequence_tagging_master/data/model/train.py fichier à executer pour entrainer le réseau.

